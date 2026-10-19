// Registration-based scheduler: the default.
//
// Each pollable queue exposes a handler returning whether it did any work.
// Handlers are spread across a fixed slot table (scheduler_registry.cpp)
// according to a relative frequency, and the loop sweeps that table. Adding a
// queue means registering a handler here rather than editing the loop.
//
// Two ordering rules sit outside the table. First, the converse thread queue
// is drained before the sweep. It holds every message delivered to this PE,
// including the ones Charm++'s _skipCldHandler still has to move into the
// prioritized CsdSchedQueue, and [expedited] entries that run straight from
// it. Running CsdSchedQueue while it is non-empty picks the "highest
// priority" message from an incomplete set (issue #258).
//
// Second, node-level work comes before the PE's own queued work (issue
// #268). The node queue holds work that any PE of the process may run, put
// there so that whichever PE is free does it soonest. In the slot table, at
// weight 1 against 16, it waited behind up to 16 local messages per sweep on
// every PE, so on a process whose PEs all had local work a node request
// waited for many work items instead of one. Classic Converse also took
// incoming node messages ahead of all local Charm++ work. So after the thread
// queue, each iteration takes a message from the node queue, or from the
// node priority queue if its priority is at least as high as the PE's own
// next message. After +node_queue_max node messages in a row (default 16),
// one iteration skips this step if the PE has local work, so a stream of
// node messages cannot starve it. The node priority queue keeps a slot in
// the table for messages of lower priority than the local work.
//
// The original hardcoded loop is in scheduler_old.cpp (+old-scheduler), and
// the randomized loop in scheduler_randomized.cpp (+randomized_msgq).

#include "scheduler_take.h"

extern std::vector<QueuePollHandler> g_handlers; //list of handlers
extern Groups g_groups; //groups of handlers by index
CpvExtern(QueuePollHandlerFn *, poll_handlers);

// Each handler is the source's take (scheduler_take.h) followed by running
// the message.
static inline bool runTaken(void *msg) {
  if (msg == nullptr) return false;
  CmiSchedulerReleaseIdle();
  CmiHandleMessage(msg);
  return true;
}

//poll converse-level thread queue
bool pollConverseThreadQueue() { return runTaken(CmiTakeThreadQueue()); }

//poll node priority queue
bool pollNodePrioQueue() { return runTaken(CmiTakeNodePrioQueue()); }

//poll thread priority queue
bool pollThreadPrioQueue() { return runTaken(CmiTakeThreadPrioQueue()); }

// node-level work before the PE's own (see top); runs ahead of the slot table
// node messages taken in a row, per PE
static thread_local int t_nodeStreak = 0;
static bool pollNodeQueuesFirst() {
  if (t_nodeStreak >= node_queue_max &&
      !QueueEmpty(CpvAccess(CsdSchedQueue))) {
    t_nodeStreak = 0; // local work's turn
    return false;
  }
  if (runTaken(CmiTakeNodeQueue()) ||
      runTaken(CmiTakeNodePrioQueueIfAhead())) {
    t_nodeStreak++;
    return true;
  }
  t_nodeStreak = 0;
  return false;
}

// Set once a sweep's progress poll has found nothing, and cleared at the
// start of every sweep. pollProgress holds about 40 of the 64 slots, so a
// sweep that finds no work used to poll the network that many times before
// the loop could go idle. On some backends each poll is a system call (the
// libfabric TCP provider's is a kevent), and everything that waits for the PE
// to be idle -- quiescence detection, CcdPROCESSOR_*_IDLE callbacks -- waited
// for a whole sweep of them: a QD round across two one-PE processes took
// 10.6 ms instead of 0.4 ms on macOS. Now an iteration makes at most one
// empty poll, idle or busy, which is the old scheduler's rate. (A busy
// iteration used to poll every progress slot it crossed before reaching
// work, about two at the default weight.)
// The flag is not a completion fact: LCI2's progress() also returns false to
// a thread that loses a device's trylock, so for the rest of that sweep it
// can mean "another thread is polling this device". That is harmless, since
// the winner delivers into this PE's thread queue, which every iteration
// drains first.
static thread_local bool t_progressFoundNothing = false;

bool pollProgress()
{
  if (t_progressFoundNothing) return false;
  //progress can deliver messages in the middle of a sweep; run them by the
  //two rules at the top instead of after the local message the sweep would
  //reach next
  if (CmiMyRank() % backend_poll_thread == 0) {
    if (comm_backend::progress())
      return pollConverseThreadQueue() || pollNodeQueuesFirst();
    t_progressFoundNothing = true;
  }
  return false; //polling progress doesn't count
}

#if CMK_TASKQUEUE
bool pollTaskQueue() { return runTaken(CmiTakeTaskQueue()); }
#endif

//called per PE, builds that PE's slot table
void CmiQueueRegisterInitThread() {
  //+old-scheduler and +randomized_msgq poll queues directly
  if (CmiSchedulerIsOld() || CmiSchedulerIsRandomized()) return;
  std::vector<std::pair<QueuePollHandlerFn, unsigned int>> handlers;
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
  //+old-scheduler and +randomized_msgq poll queues directly
  if (CmiSchedulerIsOld() || CmiSchedulerIsRandomized()) return;
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
    t_progressFoundNothing = false; //a new sweep may poll progress again
    //always deliver shmem messages (+ipc) first, every iteration rather
    //than from a slot, as the old scheduler does: the drain only moves a
    //block onto a local queue, and the sweep below then handles it
    bool ipcDone = CmiSchedulerPollIpc();
    bool workDone = pollConverseThreadQueue();
    //then node-level work, ahead of the PE's own (see top)
    if (!workDone) workDone = pollNodeQueuesFirst();
    for (unsigned t = 0; t < SCHED_TABLE_SIZE && !workDone; ++t) {
      unsigned idx = static_cast<unsigned>((loop_counter + t) & SCHED_TABLE_MASK);
      workDone = CpvAccess(poll_handlers)[idx]();
    }
    if(!workDone && !ipcDone) {
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
    t_progressFoundNothing = false; //a new sweep may poll progress again
    //same shmem drain as CsdSchedulerRegistered; a block handed to a
    //queue counts as work, so we don't return before it is handled
    bool ipcDone = CmiSchedulerPollIpc();
    bool workDone = pollConverseThreadQueue();
    //then node-level work, ahead of the PE's own (see top)
    if (!workDone) workDone = pollNodeQueuesFirst();
    for (unsigned t = 0; t < SCHED_TABLE_SIZE && !workDone; ++t) {
      unsigned idx = static_cast<unsigned>((loop_counter + t) & SCHED_TABLE_MASK);
      workDone = CpvAccess(poll_handlers)[idx]();
    }
    if(!workDone && !ipcDone) {
      //swept the whole table and every slot was empty: done
      CmiSchedulerSetIdle();
      return;
    }
    CsdPeriodic();
    loop_counter++;

  }
}
