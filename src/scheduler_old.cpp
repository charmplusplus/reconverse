// The original Reconverse scheduler: a hardcoded if/else chain over a fixed
// set of queues, selected at runtime with the +old-scheduler flag.
//
// The body below is a copy of the loops that lived in scheduler.cpp before the
// queue-registration work. Apart from the two function names, the only changes
// are the +backend_poll_freq rate handling and the shared-memory IPC drain
// (CmiSchedulerPollIpc, in both loops); everything else is untouched
// on purpose. This is the known-good fallback to compare the registered
// scheduler against, so please resist tidying it. The default implementation
// is scheduler_registered.cpp.

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
void CsdSchedulerOld() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  int loop_counter = 0;

  // +backend_poll_freq is a rate: larger means progress is polled more often.
  // This loop cannot poll more often than once per iteration, so the scale
  // saturates there. At the default the period is 1, i.e. every iteration,
  // which is what this scheduler has always done.
  int poll_period = BACKEND_POLL_FREQ_DEFAULT / backend_poll_freq;
  if (poll_period < 1) poll_period = 1;

  int tq_streak = 0; // consecutive thread-queue messages, see the thread-queue branch below
  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);

    // messages from peer processes on this host (+ipc)
    CmiSchedulerPollIpc();

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        void *msg = result.value();

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }

        // process event
        CmiHandleMessage(msg);
      }
    }

    // poll the thread queue (this PE's ConverseQueue, the multi-producer
    // single-consumer queue that other PEs and the network push into). After
    // +thread_queue_max consecutive messages from it, one message from the
    // priority queue runs instead if there is one: a sustained stream from
    // outside must not starve this PE's own queued work (self-sends, Charm++
    // local messages, thread tokens). The streak resets only when a priority
    // queue message runs or the thread queue is empty, never because the node
    // priority queue branch below ran something. The registered scheduler has
    // the same bound from its 16:16 slot weights.
    else if (!queue->empty()) {
      void *msg;
      if (tq_streak < thread_queue_max || QueueEmpty(CpvAccess(CsdSchedQueue))) {
        tq_streak++;
        // get next event (guaranteed to be there because only single consumer)
        msg = queue->pop().value();
      } else {
        tq_streak = 0;
        msg = QueueTop(CpvAccess(CsdSchedQueue));
        QueuePop(CpvAccess(CsdSchedQueue));
      }

      // release idle if necessary
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
        
      // process event
      CmiHandleMessage(msg);
    }

        // poll node prio queue
    else {
      tq_streak = 0; // the thread queue is empty
      // Check the queue length before reaching for the lock. CmiTryLock is a
      // CAS on a single process-wide cacheline, and an idle PE would otherwise
      // execute it on every loop iteration; with many PEs per process that one
      // line dominates the scheduler loop and so the latency of noticing any
      // message at all. Measured at 1 process x 120 PEs: 4201 ns per idle
      // iteration before, 88 ns after.
      if (CsdNodeQueueLenGet() > 0 &&
          CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void* msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CsdNodeQueueLenAdd(-1);
          CmiUnlock(CsvAccess(CsdNodeQueueLock));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        
          // process event
          CmiHandleMessage(msg);
        } 
        else {
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          //empty queue so check thread prio queue
          if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        
          // process event
          CmiHandleMessage(msg);
        } else {
          #if CMK_TASKQUEUE
          // Check local task queue before going idle
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }
            // Found a task in our local queue
            CmiHandleMessage(task_msg);
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

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } else {
          #if CMK_TASKQUEUE
          // Check local task queue before going idle
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }

            // Found a task in our local queue
            CmiHandleMessage(task_msg);

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
    if((CmiMyRank() % backend_poll_thread == 0) && (loop_counter++ == (poll_period - 1)))
    {
      loop_counter = 0;
      comm_backend::progress();
    }

    CsdPeriodic();

  }
}

/**
 * Similar to CsdSchedulerOld, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePollOld() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  int tq_streak = 0; // consecutive thread-queue messages, see the thread-queue branch below
  while(1){

    CsdPeriodic();

    CcdRaiseCondition(CcdSCHEDLOOP);

    // messages from peer processes on this host (+ipc)
    CmiSchedulerPollIpc();

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        void *msg = result.value();

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }

        // process event
        CmiHandleMessage(msg);

      }
    }

    // poll the thread queue (this PE's ConverseQueue, the multi-producer
    // single-consumer queue that other PEs and the network push into). After
    // +thread_queue_max consecutive messages from it, one message from the
    // priority queue runs instead if there is one: a sustained stream from
    // outside must not starve this PE's own queued work (self-sends, Charm++
    // local messages, thread tokens). The streak resets only when a priority
    // queue message runs or the thread queue is empty, never because the node
    // priority queue branch below ran something. The registered scheduler has
    // the same bound from its 16:16 slot weights.
    else if (!queue->empty()) {
      void *msg;
      if (tq_streak < thread_queue_max || QueueEmpty(CpvAccess(CsdSchedQueue))) {
        tq_streak++;
        // get next event (guaranteed to be there because only single consumer)
        msg = queue->pop().value();
      } else {
        tq_streak = 0;
        msg = QueueTop(CpvAccess(CsdSchedQueue));
        QueuePop(CpvAccess(CsdSchedQueue));
      }

      // release idle if necessary
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }

      // process event
      CmiHandleMessage(msg);

    }

    // poll node prio queue
    else {
      tq_streak = 0; // the thread queue is empty
      // Check the queue length before reaching for the lock. CmiTryLock is a
      // CAS on a single process-wide cacheline, and an idle PE would otherwise
      // execute it on every loop iteration; with many PEs per process that one
      // line dominates the scheduler loop and so the latency of noticing any
      // message at all. Measured at 1 process x 120 PEs: 4201 ns per idle
      // iteration before, 88 ns after.
      if (CsdNodeQueueLenGet() > 0 &&
          CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void *msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CsdNodeQueueLenAdd(-1);
          CmiUnlock(CsvAccess(CsdNodeQueueLock));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } 
        else {
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } 
        else {
          #if CMK_TASKQUEUE
          //because idle, check task queue
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
          
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }

            // Found a task in our local queue
            CmiHandleMessage(task_msg);

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

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } 
        else {
          #if CMK_TASKQUEUE
          //because idle, check task queue
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }

            // Found a task in our local queue
            CmiHandleMessage(task_msg);
            
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
