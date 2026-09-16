// The original Reconverse scheduler: a hardcoded if/else chain over a fixed
// set of queues, selected at runtime with the +old-scheduler flag.
//
// The body below is a copy of the loops that lived in scheduler.cpp before the
// queue-registration work. Apart from the two function names, the only changes
// are the +backend_poll_freq rate handling, the shared-memory IPC drain
// (CmiSchedulerPollIpc, in both loops) and the +thread_queue_max and
// +node_queue_max bounds on the thread queue and the two node queues;
// everything else is untouched on purpose. This is the known-good fallback to
// compare the registered scheduler against, so please resist tidying it. The
// default implementation is scheduler_registered.cpp.

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
  ConverseQueue<void *> *urgentQueue = CmiGetUrgentQueue(CmiMyRank());

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
  int nq_streak = 0; // consecutive node-level messages, see the node-queue branch below
  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);

    // messages from peer processes on this host (+ipc)
    CmiSchedulerPollIpc();

    // urgent queue first: expedited messages (see CmiSyncSendAndFreeUrgent)
    if (!urgentQueue->empty()) {
      auto r = urgentQueue->pop();
      if (r) {
        tq_streak = 0;
        nq_streak = 0;
        CmiSchedulerReleaseIdle();
        CmiHandleMessage(*r);
      }
    }
    // poll node queue (the process's converse node queue), ahead of this PE's
    // own queues. After +node_queue_max node-level messages in a row (from
    // here or the node priority queue below), one iteration skips it if the
    // PE has work of its own, so that a stream of node messages cannot starve
    // the thread queue (other PEs' messages, CmiExit's among them) or the
    // priority queue (#273). Any iteration that runs something else, or
    // nothing, resets the streak.
    else if (!nodeQueue->empty() &&
        (nq_streak < node_queue_max ||
         (queue->empty() && QueueEmpty(CpvAccess(CsdSchedQueue))))) {
      auto result = nodeQueue->pop();
      if (result) {
        nq_streak++;
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
      nq_streak = 0; // the PE's own queues get this iteration
      void *msg = nullptr;
      if (tq_streak < thread_queue_max || QueueEmpty(CpvAccess(CsdSchedQueue))) {
        // the count said non-empty; pop() can still come back empty when a
        // producer's count increment ran ahead of its enqueue, in which case
        // the message is seen on the next iteration
        auto result = queue->pop();
        if (result) { tq_streak++; msg = result.value(); }
      } else {
        tq_streak = 0;
        msg = QueueTop(CpvAccess(CsdSchedQueue));
        QueuePop(CpvAccess(CsdSchedQueue));
      }

      // release idle if necessary
      if (msg != nullptr && CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
        
      // process event
      if (msg != nullptr) CmiHandleMessage(msg);
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
      //
      // The node priority queue counts toward the node-level streak above:
      // once it is reached, the priority queue gets this iteration if it has
      // work. Before that bound, local work here ran only when the try-lock
      // happened to fail, so with one PE (no contention) it never ran (#273).
      if ((nq_streak < node_queue_max ||
           QueueEmpty(CpvAccess(CsdSchedQueue))) &&
          CsdNodeQueueLenGet() > 0 &&
          CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void* msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CsdNodeQueueLenAdd(-1);
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          nq_streak++;

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
          nq_streak = 0;
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
        // Could not acquire node queue lock, or the node-level streak gave
        // the priority queue this turn: skip to thread prio queue
        nq_streak = 0;
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
    if((CmiMyRank() % backend_poll_thread == 0 || comm_backend::threadMustPoll()) &&
       (loop_counter++ == (poll_period - 1)))
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
  ConverseQueue<void *> *urgentQueue = CmiGetUrgentQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  int tq_streak = 0; // consecutive thread-queue messages, see the thread-queue branch below
  int nq_streak = 0; // consecutive node-level messages, see the node-queue branch below
  while(1){

    CsdPeriodic();

    CcdRaiseCondition(CcdSCHEDLOOP);

    // messages from peer processes on this host (+ipc)
    CmiSchedulerPollIpc();

    // urgent queue first: expedited messages (see CmiSyncSendAndFreeUrgent)
    if (!urgentQueue->empty()) {
      auto r = urgentQueue->pop();
      if (r) {
        tq_streak = 0;
        nq_streak = 0;
        CmiSchedulerReleaseIdle();
        CmiHandleMessage(*r);
      }
    }
    // poll node queue (the process's converse node queue), ahead of this PE's
    // own queues. After +node_queue_max node-level messages in a row (from
    // here or the node priority queue below), one iteration skips it if the
    // PE has work of its own, so that a stream of node messages cannot starve
    // the thread queue (other PEs' messages, CmiExit's among them) or the
    // priority queue (#273). Any iteration that runs something else, or
    // nothing, resets the streak.
    else if (!nodeQueue->empty() &&
        (nq_streak < node_queue_max ||
         (queue->empty() && QueueEmpty(CpvAccess(CsdSchedQueue))))) {
      auto result = nodeQueue->pop();
      if (result) {
        nq_streak++;
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
      nq_streak = 0; // the PE's own queues get this iteration
      void *msg = nullptr;
      if (tq_streak < thread_queue_max || QueueEmpty(CpvAccess(CsdSchedQueue))) {
        // the count said non-empty; pop() can still come back empty when a
        // producer's count increment ran ahead of its enqueue, in which case
        // the message is seen on the next iteration
        auto result = queue->pop();
        if (result) { tq_streak++; msg = result.value(); }
      } else {
        tq_streak = 0;
        msg = QueueTop(CpvAccess(CsdSchedQueue));
        QueuePop(CpvAccess(CsdSchedQueue));
      }

      // release idle if necessary
      if (msg != nullptr && CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }

      // process event
      if (msg != nullptr) CmiHandleMessage(msg);

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
      //
      // The node priority queue counts toward the node-level streak above:
      // once it is reached, the priority queue gets this iteration if it has
      // work. Before that bound, local work here ran only when the try-lock
      // happened to fail, so with one PE (no contention) it never ran (#273).
      if ((nq_streak < node_queue_max ||
           QueueEmpty(CpvAccess(CsdSchedQueue))) &&
          CsdNodeQueueLenGet() > 0 &&
          CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void *msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CsdNodeQueueLenAdd(-1);
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          nq_streak++;

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
          nq_streak = 0;
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
        // Could not acquire node queue lock, or the node-level streak gave
        // the priority queue this turn: skip to thread prio queue
        nq_streak = 0;
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
