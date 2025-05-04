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

  //get task level queue 
  TaskQueue* taskQueue = (TaskQueue*)CpvAccess(task_q);

  void* msg = NULL;

  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        msg = result.value();
        // process event
        CmiHandleMessage(msg);

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }
      }
    } else if (taskQueue && (msg = TaskQueuePop(taskQueue))) { //taskqueue pop handles all possible queue cases arleady so we only need to check if it exists or not
      assert(msg != NULL);
      //process event 
      CmiHandleMessage(msg);
      
      // release idle if necessary 
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }

    } else if (!queue->empty()) {  // poll thread queue
      // get next event (guaranteed to be there because only single consumer)
      msg = queue->pop().value();

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
      // at this point the condition should be raised and the pe should be called. 

      // if already idle, call still idle and (maybe) long idle
      else {
        CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
        if (CmiWallTimer() - CmiGetIdleTime() > 10.0) {
          CcdRaiseCondition(CcdPROCESSOR_LONG_IDLE);
        }
      }
      // at this point the condition should be raised and the pe should be called. 

      // poll the communication layer
      comm_backend::progress();
    }

    CcdCallBacks();

    // TODO: suspend? or spin?
  }
}
// TODO: implement CsdEnqueue/Dequeue (why are these necessary?)
