#include "scheduler.h"
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include <thread>


/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {
  //printf("my rank: %d\n", CmiMyRank());
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  //get task level queue 
  TaskQueue* taskQueue = (TaskQueue*)(Cmi_taskqueues[CmiMyRank()]);

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

/**
 * Similar to CsdScheduker, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  TaskQueue* taskQueue = (TaskQueue*)(Cmi_taskqueues[CmiMyRank()]);

  void* msg = NULL; 

  while(1){

    CcdCallBacks();

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
    }

    else if (taskQueue && (msg = TaskQueuePop(taskQueue))) { //taskqueue pop handles all possible queue cases arleady so we only need to check if msg exists or not
      assert(msg != NULL);
      //process event 
      CmiHandleMessage(msg);
      
      // release idle if necessary 
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }

    }

    // poll thread queue
    else if (!queue->empty()) {
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

void CsdEnqueueGeneral(void *Message, int strategy, int priobits, int *prioptr){
  CmiPushPE(CmiMyPe(), sizeof(Message), Message);
}

// TODO: implement CsdEnqueue/Dequeue (why are these necessary?)
