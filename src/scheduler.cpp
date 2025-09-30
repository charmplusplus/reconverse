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

    // the processor is idle
    else {
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
      // poll the communication layer
      comm_backend::progress();
    }

    CcdCallBacks();

    // TODO: suspend? or spin?
  }
}

/**
 * Similar to CsdScheduker, but return when the queues
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
    else if (!CsvAccess(CsdNodeQueue).empty()) {
      CmiLock(CsvAccess(CsdNodeQueueLock));
      auto result = CsvAccess(CsdNodeQueue).top();
      CsvAccess(CsdNodeQueue).pop();
      CmiUnlock(CsvAccess(CsdNodeQueueLock));
      void *msg = result.message;
      // process event
      CmiHandleMessage(msg);

      // release idle if necessary
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    //poll thread prio queue
    else if (!CpvAccess(CsdSchedQueue).empty()) {
      auto result = CpvAccess(CsdSchedQueue).top();
      CpvAccess(CsdSchedQueue).pop();
      void *msg = result.message;

      // process event
      CmiHandleMessage(msg);

      // release idle if necessary
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
    }

    else {
      comm_backend::progress();
      break; //break when queues are empty
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
              q.push(MessagePriorityPair((void*)Message, 0));
              break;
            case CQS_QUEUEING_IFIFO:
            case CQS_QUEUEING_ILIFO:
              iprio=prioptr[0]+(1U<<(8*sizeof(unsigned int)-1));
              q.push(MessagePriorityPair((void*)Message, iprio));
              break;
            case CQS_QUEUEING_LFIFO:
            case CQS_QUEUEING_LLIFO:
              lprio = ((long long*)prioptr)[0] + (1ULL<<(8*sizeof(long long)-1));
              q.push(MessagePriorityPair((void*)Message, lprio));
              break;
            default:
              CmiAbort("CqsEnqueueGeneral: invalid queueing strategy (bitvectors not supported yet)\n");
          }
}
