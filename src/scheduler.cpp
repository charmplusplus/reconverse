#include "scheduler.h"
#if CMK_TASKQUEUE
#include "taskqueue.h"
CpvExtern(TaskQueue, CsdTaskQueue);
#endif

extern std::vector<QueuePollHandler> g_handlers; //list of handlers
extern Groups g_groups; //groups of handlers by index
CpvExtern(QueuePollHandlerFn *, poll_handlers);

// One trip around the polling table is ARRAY_SIZE scheduler iterations, so
// re-balance every ADAPT_PERIOD_CYCLES * ARRAY_SIZE iterations.
#define ADAPT_INTERVAL ((uint64_t)ARRAY_SIZE * ADAPT_PERIOD_CYCLES)

// Sweep the table starting at `start`, stopping as soon as a handler reports it
// did work, so a message never waits for the loop counter to rotate back around
// to its slot.  Whichever queue produced the message gets the credit, which is
// what the adaptation later apportions slots from.
static inline bool pollOnce(PollTable *pt, uint64_t start) {
  for (unsigned t = 0; t < ARRAY_SIZE; ++t) {
    unsigned idx = static_cast<unsigned>((start + t) & (ARRAY_SIZE - 1));
    int owner = pt->owner[idx];
    if (owner >= 0) pt->polls[owner]++;
    if (pt->slots[idx]()) {
      if (owner >= 0) {
        pt->counts[owner]++;
        pt->lifetime[owner]++;
      }
      return true;
    }
  }
  return false;
}

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
      releaseIdle();
      // process event
      CmiHandleMessage(msg);
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
    releaseIdle();
    // process event
    CmiHandleMessage(msg);
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
      releaseIdle();
      // process event
      CmiHandleMessage(msg);
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
    releaseIdle();
    // process event
    CmiHandleMessage(msg);
    return true;
  }
  return false;
}

bool pollProgress()
{
  if(CmiMyRank() % backend_poll_thread == 0) comm_backend::progress();
  return false; //polling progress doesn't count
}

#if CMK_TASKQUEUE
bool pollTaskQueue() {
  void *task_msg = TaskQueuePopLocal();
  if (task_msg != nullptr) {
    releaseIdle();
    CmiHandleMessage(task_msg);
    return true;
  }
  return false;
}
#endif

void CmiQueueRegisterInitThread(char **argv) {
  std::vector<std::pair<QueuePollHandlerFn, unsigned int>> handlers;
  std::vector<std::string> names;

  handlers.push_back(std::make_pair(pollConverseNodeQueue, 1));
  names.push_back("nodeq");
  handlers.push_back(std::make_pair(pollConverseThreadQueue, 16));
  names.push_back("threadq");
  handlers.push_back(std::make_pair(pollNodePrioQueue, 1));
  names.push_back("nodeprio");
  handlers.push_back(std::make_pair(pollThreadPrioQueue, 16));
  names.push_back("threadprio");

  // Within a single process there is nothing for the network backend to
  // progress, and pollProgress never reports work, so it would sit in the
  // table holding its guaranteed slot and cost a call per trip.
  // +no_progress_polling leaves it unregistered.
  if (!CmiGetArgFlag(argv, "+no_progress_polling")) {
    handlers.push_back(std::make_pair(pollProgress, 4));
    names.push_back("progress");
  }
#if CMK_TASKQUEUE
  handlers.push_back(std::make_pair(pollTaskQueue, 1));
  names.push_back("taskq");
#endif
  add_list_of_handlers(handlers, names, argv);
}

//will add queue polling functions
//called at node level (before threads created)
void CmiQueueRegisterInit() {
  add_handler(pollConverseNodeQueue, 1);
  add_handler(pollConverseThreadQueue, 16);
  add_handler(pollNodePrioQueue, 1);
  add_handler(pollThreadPrioQueue, 16);
  add_handler(pollProgress, backend_poll_freq);
#if CMK_TASKQUEUE
  add_handler(pollTaskQueue, 1);
#endif
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
    PollTable *pt = CpvAccess(poll_table);
    bool workDone = pollOnce(pt, loop_counter);
    if(!workDone) {
      setIdle();
    }
    CcdCallBacks();
    loop_counter++;

    // Re-apportion slots from what each queue actually delivered.
    if (pt->adaptive && (loop_counter % ADAPT_INTERVAL) == 0) {
      pollTableAdapt(pt);
    }

  }
}

/**
 * Similar to CsdScheduler, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  uint64_t loop_counter = 0;

  while(1){

    CcdRaiseCondition(CcdSCHEDLOOP);
    PollTable *pt = CpvAccess(poll_table);
    bool workDone = pollOnce(pt, loop_counter);
    if(!workDone) {
      //swept the whole table and every slot was empty: done
      setIdle();
      return;
    }
    CcdCallBacks();
    loop_counter++;

    if (pt->adaptive && (loop_counter % ADAPT_INTERVAL) == 0) {
      pollTableAdapt(pt);
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
