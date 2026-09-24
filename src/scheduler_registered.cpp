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

// Sweep the table starting at `start`, stopping as soon as a handler reports it
// did work, so a message never waits for the loop counter to rotate back around
// to its slot.  Whichever queue produced the message gets the credit, which is
// what the adaptation later apportions slots from.
static inline bool pollOnce(PollTable *pt, uint64_t start) {
  for (unsigned t = 0; t < SCHED_TABLE_SIZE; ++t) {
    unsigned idx = static_cast<unsigned>((start + t) & SCHED_TABLE_MASK);
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
  // Nothing anywhere: this sweep probed every slot once, charging each queue
  // once per slot it holds.  Undo that if idle sweeps are not to count.
  if (pt->skipIdle) {
    for (size_t i = 0; i < pt->polls.size(); ++i) pt->polls[i] -= pt->slotsOf[i];
  }
  return false;
}

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
  if (CmiMyRank() % backend_poll_thread != 0) return false;
  if (!comm_backend::progress()) return false;
  // Under adaptation, network progress counts as work whenever the backend
  // completed something, so it earns table slots like a queue that delivered
  // a message.  Reporting nothing, as before, meant every adaptive policy cut
  // it to its one-slot floor regardless of how much the application depends
  // on the network.  A static table keeps the old behaviour.
  PollTable *pt = CpvAccess(poll_table);
  return pt && pt->adaptive;
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
void CmiQueueRegisterInitThread(char **argv) {
  if (CmiSchedulerIsOld()) return; //+old-scheduler polls queues directly
  std::vector<std::pair<QueuePollHandlerFn, unsigned int>> handlers;
  std::vector<std::string> names;

  handlers.push_back(std::make_pair(pollConverseNodeQueue, 1));
  names.push_back("nodeq");
  handlers.push_back(std::make_pair(pollSelfQueue, 16));
  names.push_back("selfq");
  handlers.push_back(std::make_pair(pollConverseThreadQueue, 16));
  names.push_back("threadq");
  handlers.push_back(std::make_pair(pollNodePrioQueue, 1));
  names.push_back("nodeprio");
  handlers.push_back(std::make_pair(pollThreadPrioQueue, 16));
  names.push_back("threadprio");

  // Within a single process there is nothing for the network backend to
  // progress, so this handler would only hold its guaranteed slot and cost a
  // call per trip; +no_progress_polling leaves it unregistered.
  if (!CmiGetArgFlag(argv, "+no_progress_polling")) {
    handlers.push_back(std::make_pair(pollProgress, backend_poll_freq));
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
  uint64_t until_adapt = CpvAccess(poll_table)->adaptPeriod;

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
      CmiSchedulerSetIdle();
    }
    CsdPeriodic();
    loop_counter++;

    // Re-apportion slots from what each queue actually delivered, every
    // pt->adaptPeriod iterations.  A countdown keeps a runtime divisor out of
    // the loop; it fires on the same iterations as a modulo test would.
    if (pt->adaptive && --until_adapt == 0) {
      until_adapt = pt->adaptPeriod;
      pollTableAdapt(pt);
    }

  }
}

/**
 * Similar to CsdSchedulerRegistered, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePollRegistered() {
  uint64_t loop_counter = 0;
  uint64_t until_adapt = CpvAccess(poll_table)->adaptPeriod;

  while(1){

    CcdRaiseCondition(CcdSCHEDLOOP);
    PollTable *pt = CpvAccess(poll_table);
    bool workDone = pollOnce(pt, loop_counter);
    if(!workDone) {
      //swept the whole table and every slot was empty: done
      CmiSchedulerSetIdle();
      return;
    }
    CsdPeriodic();
    loop_counter++;

    if (pt->adaptive && --until_adapt == 0) {
      until_adapt = pt->adaptPeriod;
      pollTableAdapt(pt);
    }

  }
}
