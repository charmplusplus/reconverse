#include "scheduler.h"

extern std::vector<QueuePollHandler> g_handlers; //list of handlers
extern Groups g_groups; //groups of handlers by index

static inline void releaseIdle() {
  if (CmiGetIdle()) {
    CmiSetIdle(false);
    CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
  }
}

static inline void setIdle() {
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

//poll converse-level node queue
bool pollConverseNodeQueue() {
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();
  if (!nodeQueue->empty()) {
    auto result = nodeQueue->pop();
    if (result) {
      void *msg = result.value();
      // process event
      CmiHandleMessage(msg);
      releaseIdle();
      return true;
    }
  }
  return false;
}

//poll converse-level thread queue
bool pollConverseThreadQueue() {
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());
  if (!queue->empty()) {
    // get next event (guaranteed to be there because only single consumer)
    void *msg = queue->pop().value();
    // process event
    CmiHandleMessage(msg);
    releaseIdle();
    return true;
  }
  return false;
}

//poll node priority queue
bool pollNodePrioQueue() {
  // Try to acquire lock without blocking
  if (CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
    if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
      void *msg = QueueTop(CsvAccess(CsdNodeQueue));
      QueuePop(CsvAccess(CsdNodeQueue));
      CmiUnlock(CsvAccess(CsdNodeQueueLock));
      // process event
      CmiHandleMessage(msg);
      releaseIdle();
      return true;
    } else {
      CmiUnlock(CsvAccess(CsdNodeQueueLock));
    }
  }
  return false;
}

//poll thread priority queue
bool pollThreadPrioQueue() {
  if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
    void *msg = QueueTop(CpvAccess(CsdSchedQueue));
    QueuePop(CpvAccess(CsdSchedQueue));
    // process event
    CmiHandleMessage(msg);
    releaseIdle();
    return true;
  }
  return false;
}

bool pollProgress()
{
  if(CmiMyRank() % backend_poll_thread == 0) comm_backend::progress();
  return false; //polling progress doesn't count
}

//will add queue polling functions
//called at node level (before threads created)
void CmiQueueRegisterInit() {
  add_handler(pollConverseNodeQueue, 8);
  add_handler(pollConverseThreadQueue, 1);
  add_handler(pollNodePrioQueue, 16);
  add_handler(pollThreadPrioQueue, 1);
  add_handler(pollProgress, backend_poll_freq);
}

/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {

  uint64_t loop_counter = 0;

  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);
    //always deliver shmem messages first
    #ifdef CMK_USE_SHMEM
        CmiIpcBlock* block = CmiPopIpcBlock(CsvAccess(coreIpcManager_));
        if (block != nullptr) {
          CmiDeliverIpcBlockMsg(block);
        }
    #endif
    //poll queues
    unsigned idx = static_cast<unsigned>(loop_counter & 63ULL);
    bool workDone = false;
    for (auto fn : g_groups[idx]) {
        workDone |= fn();
    }
    if(!workDone) {
      setIdle();
    }
    CcdCallBacks();
    loop_counter++;

  }
}

/**
 * Similar to CsdScheduler, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  uint64_t loop_counter = 0;

  while(1){

    CcdCallBacks();
    CcdRaiseCondition(CcdSCHEDLOOP);
    //poll queues
    unsigned idx = static_cast<unsigned>(loop_counter & 63ULL);
    bool workDone = false;
    for (auto fn : g_groups[idx]) {
        workDone |= fn();
    }
    if(!workDone) {
      setIdle();
      break;
    }
    loop_counter++;
    
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

