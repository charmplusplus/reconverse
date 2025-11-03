#include "scheduler.h"
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include <thread>

/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  int loop_counter = 0;

  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        void *msg = result.value();
        // process event
        CmiHandleMessage(msg);

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }
      }
    }

    // poll thread queue
    else if (!queue->empty()) {
      // get next event (guaranteed to be there because only single consumer)
      void *msg = queue->pop().value();

      // process event
      CmiHandleMessage(msg);

      // release idle if necessary
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
        }
      }
    }
    if((CmiMyRank() % backend_poll_thread == 0) && (loop_counter++ == (backend_poll_freq - 1)))
    {
      loop_counter = 0;
      comm_backend::progress();
    }

    CcdCallBacks();

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

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        void *msg = result.value();
        // process event
        CmiHandleMessage(msg);

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }
      }
    }

    // poll thread queue
    else if (!queue->empty()) {
      // get next event (guaranteed to be there because only single consumer)
      void *msg = queue->pop().value();

      // process event
      CmiHandleMessage(msg);

      // release idle if necessary
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
        } else {
          comm_backend::progress();
          break; //break when queues are empty
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
          comm_backend::progress();
          break; //break when queues are empty
        }
      }
    }
  }
}

int CsdScheduler(int maxmsgs){
  if (maxmsgs < 0) {
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
              iprio=prioptr[0]+(1U<<(8*sizeof(unsigned int)-1));
              QueuePush(q, Message, iprio);
              break;
            case CQS_QUEUEING_LFIFO:
            case CQS_QUEUEING_LLIFO:
              lprio = ((long long*)prioptr)[0] + (1ULL<<(8*sizeof(long long)-1));
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

