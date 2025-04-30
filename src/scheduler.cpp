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
      QueueResult result = nodeQueue->pop();
      if (result) {
        void *msg = result.msg;
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
      void *msg = queue->pop();

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

// TODO: implement CsdEnqueue/Dequeue (why are these necessary?)
