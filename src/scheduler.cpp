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
  // Throttle CcdCallBacks: it calls CmiWallTimer + walks the periodic-callback
  // heap on every iteration. The fastest periodic level is 1 ms, so even at
  // ~1 µs/iter, firing it every 256 iters is well within the tightest budget
  // and removes the timer/heap walk from the hot path. Profile showed
  // getCurrentTime + ccd_heap_update were ~2-3% of total CPU.
  constexpr int CCD_CALLBACKS_THROTTLE = 256;
  int ccd_callbacks_counter = 0;

  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);

    #ifdef CMK_USE_SHMEM
        CmiIpcBlock* block = CmiPopIpcBlock(CsvAccess(coreIpcManager_));
        if (block != nullptr) {
          CmiDeliverIpcBlockMsg(block);
        }
    #endif

    // poll node queue
    // Profile showed moodycamel::size_approx (called by empty()) was the
    // single hottest symbol. Skip the empty-check entirely; try_dequeue
    // already returns false fast when the queue is empty, and one call is
    // strictly cheaper than empty() + pop().
    if (auto result = nodeQueue->pop()) {
      void *msg = result.value();
      CmiHandleMessage(msg);
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    // poll thread queue (same single-pop-no-empty-check pattern)
    else if (auto result = queue->pop()) {
      void *msg = result.value();
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

    // poll node queue (single-pop, no empty-check — see CsdScheduler)
    if (auto result = nodeQueue->pop()) {
      void *msg = result.value();
      CmiHandleMessage(msg);
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    // poll thread queue
    else if (auto result = queue->pop()) {
      void *msg = result.value();
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

