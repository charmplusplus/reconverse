#include "scheduler.h"
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include "taskqueue.h"
#include <thread>

CpvExtern(TaskQueue, CsdTaskQueue);

/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  int loop_counter = 0;
  // Throttle two pieces of per-iteration housekeeping:
  //
  //   CCD_CALLBACKS_THROTTLE — CcdCallBacks walks the periodic-callback heap
  //   and calls CmiWallTimer. Fastest periodic level is 1 ms; at ~1 µs/iter,
  //   256 iters keeps us well inside the budget.
  //
  //   SCHEDLOOP_THROTTLE — CcdRaiseCondition(CcdSCHEDLOOP) fires the
  //   condcb_keep list for that condition, which on a CUDA build includes
  //   hapiPollEvents (driver cudaEventQuery). HAPI tolerates a few-µs
  //   polling delay (kernel completion sees it on the next iter). At
  //   ~1 µs/iter, throttle=16 means HAPI polls every ~16 µs — well within
  //   tolerance for any realistic GPU workload. Profile still showed
  //   call_cblist_keep + vector::size at ~6% even with throttle=4; this
  //   bump should cut both another 4x.
  constexpr int CCD_CALLBACKS_THROTTLE = 256;
  constexpr int SCHEDLOOP_THROTTLE = 16;
  int ccd_callbacks_counter = 0;
  int schedloop_counter = 0;

  while (CmiStopFlag() == 0) {

    if (++schedloop_counter >= SCHEDLOOP_THROTTLE) {
      schedloop_counter = 0;
      CcdRaiseCondition(CcdSCHEDLOOP);
    }

    #ifdef CMK_USE_SHMEM
        CmiIpcBlock* block = CmiPopIpcBlock(CsvAccess(coreIpcManager_));
        if (block != nullptr) {
          CmiDeliverIpcBlockMsg(block);
        }
    #endif

    // poll node queue — moodycamel under the hood, but try_pop_ptr returns
    // a raw void* (nullptr = empty) so we skip std::optional construction
    // on every poll (~3.7% of CPU).
    if (void *msg = nodeQueue->try_pop_ptr()) {
      CmiHandleMessage(msg);
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    // poll thread queue — bounded MPSC ring with the same pointer-return
    // fast path.
    else if (void *msg = queue->try_pop_ptr()) {
      CmiHandleMessage(msg);
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

        // poll node prio queue
    else {
      // Fast-empty short-circuit: if both priority queues are observed empty
      // by their cached size field, skip the trylock + nested empty walks
      // entirely. In workloads that never send prioritized messages (which is
      // most of them), this collapses the bottom of the scheduler loop to
      // two loads + a branch. Profile showed pthread_mutex_trylock alone was
      // ~2% of CPU. A concurrent push we miss here gets picked up on the
      // next iteration; QueuePush updates size under whatever lock the
      // caller holds.
      Queue csdNodeQ = CsvAccess(CsdNodeQueue);
      Queue csdSchedQ = CpvAccess(CsdSchedQueue);
      if (QueueFastEmpty(csdNodeQ) && QueueFastEmpty(csdSchedQ)) {
        #if CMK_TASKQUEUE
        void *task_msg = TaskQueuePopLocal();
        if (task_msg != NULL) {
          CmiHandleMessage(task_msg);
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        } else
        #endif
        {
          if (!CmiGetIdle()) {
            CmiSetIdle(true);
            CmiSetIdleTime(CmiWallTimer());
            CcdRaiseCondition(CcdPROCESSOR_BEGIN_IDLE);
          } else {
            CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
            // Throttle the long-idle wallclock check. Firing CmiWallTimer
            // every iteration while spinning was ~1.7% of CPU in the profile;
            // a 10-second threshold tolerates a few-hundred-iter delay just
            // fine.
            if (ccd_callbacks_counter == 0
                && CmiWallTimer() - CmiGetIdleTime() > 10.0) {
              CcdRaiseCondition(CcdPROCESSOR_LONG_IDLE);
            }
          }
        }
      }
      // Try to acquire lock without blocking
      else if (CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void* msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          // process event
          CmiHandleMessage(msg);

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        }
        else {
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          //empty queue so check thread prio queue
          if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // process event
          CmiHandleMessage(msg);

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        } else {
          #if CMK_TASKQUEUE
          // Check local task queue before going idle
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            // Found a task in our local queue
            CmiHandleMessage(task_msg);
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }
          } else {
          #endif
            // the processor is idle
            // if not already idle, set idle and raise condition
            if (!CmiGetIdle()) {
              CmiSetIdle(true);
              CmiSetIdleTime(CmiWallTimer());
              CcdRaiseCondition(CcdPROCESSOR_BEGIN_IDLE);
            }
            // if already idle, call still idle and (maybe) long idle
            else {
              CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
              if (CmiWallTimer() - CmiGetIdleTime() > 10.0) {
                CcdRaiseCondition(CcdPROCESSOR_LONG_IDLE);
              }
            }
          #if CMK_TASKQUEUE
          }
          #endif
        }
        }        
      } 
      else {
        // Could not acquire node queue lock, skip to thread prio queue
        if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // process event
          CmiHandleMessage(msg);

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        } else {
          #if CMK_TASKQUEUE
          // Check local task queue before going idle
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            // Found a task in our local queue
            CmiHandleMessage(task_msg);
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }
          } else {
          #endif
            // the processor is idle
            // if not already idle, set idle and raise condition
            if (!CmiGetIdle()) {
              CmiSetIdle(true);
              CmiSetIdleTime(CmiWallTimer());
              CcdRaiseCondition(CcdPROCESSOR_BEGIN_IDLE);
            }
            // if already idle, call still idle and (maybe) long idle
            else {
              CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
              if (CmiWallTimer() - CmiGetIdleTime() > 10.0) {
                CcdRaiseCondition(CcdPROCESSOR_LONG_IDLE);
              }
            }
          #if CMK_TASKQUEUE
          }
          #endif
        }
      }
    }
    if((CmiMyRank() % backend_poll_thread == 0) && (loop_counter++ == (backend_poll_freq - 1)))
    {
      loop_counter = 0;
      comm_backend::progress();
    }

    if (++ccd_callbacks_counter >= CCD_CALLBACKS_THROTTLE) {
      ccd_callbacks_counter = 0;
      CcdCallBacks();
    }

  }
}

/**
 * Similar to CsdScheduler, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  while(1){

    CcdCallBacks();

    CcdRaiseCondition(CcdSCHEDLOOP);

    // poll node queue — pointer-return fast path (no std::optional ctor)
    if (void *msg = nodeQueue->try_pop_ptr()) {
      CmiHandleMessage(msg);
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    // poll thread queue — bounded MPSC ring fast path
    else if (void *msg = queue->try_pop_ptr()) {
      CmiHandleMessage(msg);
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    // poll node prio queue
    else {
      // Try to acquire lock without blocking
      if (CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void *msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          // process event
          CmiHandleMessage(msg);

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        } 
        else {
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // process event
          CmiHandleMessage(msg);

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        } 
        else {
          #if CMK_TASKQUEUE
          //because idle, check task queue
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            // Found a task in our local queue
            CmiHandleMessage(task_msg);
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }
          }
          else
          {
          #endif
            comm_backend::progress();
            break; //break when queues are empty
          #if CMK_TASKQUEUE
          }
          #endif
        }
        }
      } 
      else {
        // Could not acquire node queue lock, skip to thread prio queue
        if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // process event
          CmiHandleMessage(msg);

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        } 
        else {
          #if CMK_TASKQUEUE
          //because idle, check task queue
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            // Found a task in our local queue
            CmiHandleMessage(task_msg);
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }
          }
          else
          {
          #endif
            comm_backend::progress();
            break; //break when queues are empty
          #if CMK_TASKQUEUE
          }
          #endif
        }
      }
    }
  }
}

int CsdScheduler(int maxmsgs){
  if (maxmsgs < 0) {
    //reset stop flag
    CmiGetState()->stopFlag = 0;
    CsdScheduler(); //equivalent to CsdScheduleForever in old converse
  }
  else CsdSchedulePoll(); //not implementing CsdScheduleCount
  return 0;
  
}

void CqsEnqueueGeneral(Queue q, void *Message, int strategy, int priobits,
                         unsigned int *prioptr){
          int iprio;
          long long lprio;
          switch (strategy){ //for now everything is FIFO
            case CQS_QUEUEING_FIFO:
            case CQS_QUEUEING_LIFO:
              QueuePush(q, Message, 0);
              break;
            case CQS_QUEUEING_IFIFO:
            case CQS_QUEUEING_ILIFO:
              iprio=prioptr[0];
              QueuePush(q, Message, iprio);
              break;
            case CQS_QUEUEING_LFIFO:
            case CQS_QUEUEING_LLIFO:
              lprio = ((long long*)prioptr)[0];
              QueuePush(q, Message, lprio);
              break;
            default:
              // unknown strategy, default to FIFO
              QueuePush(q, Message, 0);
              break;
          }
}

//network progress
void CmiNetworkProgress(){
  comm_backend::progress();
}

