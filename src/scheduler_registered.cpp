// Registration-based scheduler: the default.
//
// Each pollable queue exposes a handler returning whether it did any work.
// Handlers are spread across a fixed slot table (scheduler_registry.cpp)
// according to a relative frequency, and the loop sweeps that table. Adding a
// queue means registering a handler here rather than editing the loop.
//
// The original hardcoded loop is in scheduler_old.cpp (+old-scheduler).

#include "scheduler.h"
#if CMK_TASKQUEUE
#include "taskqueue.h"
CpvExtern(TaskQueue, CsdTaskQueue);
#endif

extern std::vector<QueuePollHandler> g_handlers; //list of handlers
extern Groups g_groups; //groups of handlers by index
CpvExtern(QueuePollHandlerFn *, poll_handlers);

//poll converse-level node queue
bool pollConverseNodeQueue() {
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();
  if (!nodeQueue->empty()) {
    auto result = nodeQueue->pop();
    if (result) {
      void *msg = result.value();
      CmiSchedulerReleaseIdle();
      // process event
      CmiHandleMessage(msg);
      return true;
    }
  }
  return false;
}

//poll this PE's self queue
bool pollSelfQueue() {
  ConverseSelfQueue<void *> *selfQueue = CmiGetSelfQueue();
  if (!selfQueue->empty()) {
    void *msg = selfQueue->pop();
    CmiSchedulerReleaseIdle();
    // process event
    CmiHandleMessage(msg);
    return true;
  }
  return false;
}

//poll converse-level thread queue
bool pollConverseThreadQueue() {
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());
  if (!queue->empty()) {
    // get next event (guaranteed to be there because only single consumer)
    void *msg = queue->pop().value();
    CmiSchedulerReleaseIdle();
    // process event
    CmiHandleMessage(msg);
    return true;
  }
  return false;
}

//poll node priority queue
bool pollNodePrioQueue() {
  // Check the queue length before reaching for the lock. CmiTryLock is a
  // CAS on a single process-wide cacheline, and an idle PE would otherwise
  // execute it on every loop iteration; with many PEs per process that one
  // line dominates the scheduler loop and so the latency of noticing any
  // message at all. Measured at 1 process x 120 PEs: 4201 ns per idle
  // iteration before, 88 ns after.
  if (CsvAccess(CsdNodeQueueLen).load(std::memory_order_relaxed) > 0 &&
      CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
    if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
      void *msg = QueueTop(CsvAccess(CsdNodeQueue));
      QueuePop(CsvAccess(CsdNodeQueue));
      CsvAccess(CsdNodeQueueLen).fetch_sub(1, std::memory_order_relaxed);
      CmiUnlock(CsvAccess(CsdNodeQueueLock));
      CmiSchedulerReleaseIdle();
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
    CmiSchedulerReleaseIdle();
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
    CmiSchedulerReleaseIdle();
    CmiHandleMessage(task_msg);
    return true;
  }
  return false;
}
#endif

//called per PE, builds that PE's slot table
void CmiQueueRegisterInitThread() {
  if (CmiSchedulerIsOld()) return; //+old-scheduler polls queues directly
  std::vector<std::pair<QueuePollHandlerFn, unsigned int>> handlers;
  handlers.push_back(std::make_pair(pollConverseNodeQueue, 1));
  handlers.push_back(std::make_pair(pollSelfQueue, 16));
  handlers.push_back(std::make_pair(pollConverseThreadQueue, 16));
  handlers.push_back(std::make_pair(pollNodePrioQueue, 1));
  handlers.push_back(std::make_pair(pollThreadPrioQueue, 16));
  handlers.push_back(std::make_pair(pollProgress, backend_poll_freq));
#if CMK_TASKQUEUE
  handlers.push_back(std::make_pair(pollTaskQueue, 1));
#endif
  add_list_of_handlers(handlers);
}

//will add queue polling functions
//called at node level (before threads created)
void CmiQueueRegisterInit() {
  if (CmiSchedulerIsOld()) return; //+old-scheduler polls queues directly
  add_handler(pollConverseNodeQueue, 1);
  add_handler(pollSelfQueue, 16);
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
void CsdSchedulerRegistered() {

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
    //poll queues: sweep forward from idx until work is found or a full
    //cycle of the table has been checked, so a message doesn't have to
    //wait for loop_counter to rotate back around to its slot
    bool workDone = false;
    for (unsigned t = 0; t < SCHED_TABLE_SIZE && !workDone; ++t) {
      unsigned idx = static_cast<unsigned>((loop_counter + t) & SCHED_TABLE_MASK);
      workDone = CpvAccess(poll_handlers)[idx]();
    }
    if(!workDone) {
      CmiSchedulerSetIdle();
    }
    CsdPeriodic();
    loop_counter++;

  }
}

/**
 * Similar to CsdSchedulerRegistered, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePollRegistered() {
  uint64_t loop_counter = 0;

  while(1){

    CcdRaiseCondition(CcdSCHEDLOOP);
    //poll queues: sweep the full table before concluding it's empty, so
    //a message doesn't have to wait for loop_counter to rotate back
    //around to its slot
    bool workDone = false;
    for (unsigned t = 0; t < SCHED_TABLE_SIZE && !workDone; ++t) {
      unsigned idx = static_cast<unsigned>((loop_counter + t) & SCHED_TABLE_MASK);
      workDone = CpvAccess(poll_handlers)[idx]();
    }
    if(!workDone) {
      //swept the whole table and every slot was empty: done
      CmiSchedulerSetIdle();
      return;
    }
    CsdPeriodic();
    loop_counter++;

  }
}
