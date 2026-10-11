// Registration-based scheduler: the default.
//
// Each pollable queue exposes a handler returning whether it did any work.
// Handlers are spread across a fixed slot table (scheduler_registry.cpp)
// according to a relative frequency, and the loop sweeps that table. Adding a
// queue means registering a handler here rather than editing the loop.
//
// One ordering rule sits outside the table: the converse thread queue is
// drained before the sweep. It holds every message delivered to this PE,
// including the ones Charm++'s _skipCldHandler still has to move into the
// prioritized CsdSchedQueue, and [expedited] entries that run straight from
// it. Running CsdSchedQueue while it is non-empty picks the "highest
// priority" message from an incomplete set (issue #258).
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

//poll converse-level node queue
bool pollConverseNodeQueue() { return runTaken(CmiTakeNodeQueue()); }

//poll this PE's self queue
bool pollSelfQueue() { return runTaken(CmiTakeSelfQueue()); }

//poll converse-level thread queue
bool pollConverseThreadQueue() { return runTaken(CmiTakeThreadQueue()); }

//poll node priority queue
bool pollNodePrioQueue() { return runTaken(CmiTakeNodePrioQueue()); }

//poll thread priority queue
bool pollThreadPrioQueue() { return runTaken(CmiTakeThreadPrioQueue()); }

bool pollProgress()
{
  if(CmiMyRank() % backend_poll_thread == 0) comm_backend::progress();
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
  //+old-scheduler and +randomized_msgq poll queues directly
  if (CmiSchedulerIsOld() || CmiSchedulerIsRandomized()) return;
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
    //always deliver shmem messages (+ipc) first, every iteration rather
    //than from a slot, as the old scheduler does: the drain only moves a
    //block onto a local queue, and the sweep below then handles it
    bool ipcDone = CmiSchedulerPollIpc();
    //poll queues: sweep forward from idx until work is found or a full
    //cycle of the table has been checked, so a message doesn't have to
    //wait for loop_counter to rotate back around to its slot
    //the thread queue feeds CsdSchedQueue, so drain it first (see top)
    bool workDone = pollConverseThreadQueue();
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
    //same shmem drain as CsdSchedulerRegistered; a block handed to a
    //queue counts as work, so we don't return before it is handled
    bool ipcDone = CmiSchedulerPollIpc();
    //poll queues: sweep the full table before concluding it's empty, so
    //a message doesn't have to wait for loop_counter to rotate back
    //around to its slot
    //the thread queue feeds CsdSchedQueue, so drain it first (see top)
    bool workDone = pollConverseThreadQueue();
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
