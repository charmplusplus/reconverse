// Randomized scheduler: selected at runtime with +randomized_msgq.
//
// A debugging mode for shaking out message-order races. Each loop iteration
//   1. moves one shared-memory IPC block onto its local queue and runs the
//      backend progress, as the registered loop does;
//   2. drains every message source -- node queue, self queue, thread queue,
//      node priority queue, thread priority queue (CsdSchedQueue) and, with
//      CMK_TASKQUEUE, the task queue -- into one per-PE pool;
//   3. removes a uniformly random message from the pool and runs it.
// Nothing bypasses the pool: messages the network just delivered, entries
// Charm++ marks [expedited], and messages a handler re-enqueues (for example
// a thread-queue message that Charm++ moves into CsdSchedQueue, which the
// next iteration drains again) all wait in the pool and are drawn at random.
// Priorities and FIFO order are therefore not respected.
//
// Each drain takes at most the number of messages its source held when the
// drain started, so a source that refills while being drained cannot keep
// the loop from reaching the draw.
//
// The draw uses a per-PE std::mt19937_64 seeded with the base seed
// (+randomized_seed <N>, or the wall clock) mixed with the PE number, so PEs
// draw different sequences and a given seed repeats them. Message arrival
// timing is not controlled, so a seed makes the draws repeatable, not the run.
//
// Cost when the flag is off: none. CsdScheduler()/CsdSchedulePoll() choose
// this loop or the registered one once per call, outside the loops, and the
// registered loop and its poll handlers contain no test of the flag.

#include "scheduler_take.h"
#include <random>

namespace {

struct RandomizedState {
  std::vector<void *> pool;
  std::mt19937_64 rng;
};

// splitmix64 finalizer: spreads consecutive PE numbers over the seed space
uint64_t mixSeed(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// one per PE thread, created on the PE's first scheduler call
thread_local RandomizedState *t_state = nullptr;

RandomizedState &state() {
  if (t_state == nullptr) {
    t_state = new RandomizedState();
    t_state->rng.seed(_Cmi_randomizedSeed ^ mixSeed((uint64_t)CmiMyPe()));
  }
  return *t_state;
}

// Moves up to `bound` messages from one source into the pool.
template <typename Take>
inline bool drainInto(std::vector<void *> &pool, size_t bound, Take take) {
  bool took = false;
  for (size_t i = 0; i < bound; ++i) {
    void *msg = take();
    if (msg == nullptr) break;
    pool.push_back(msg);
    took = true;
  }
  return took;
}

// One iteration's intake: IPC, progress, then every source. Returns whether
// any message moved.
bool fillPool(std::vector<void *> &pool) {
  bool took = CmiSchedulerPollIpc();
  if (CmiMyRank() % backend_poll_thread == 0) comm_backend::progress();
  took |= drainInto(pool, CmiGetNodeQueue()->size(), CmiTakeNodeQueue);
  took |= drainInto(pool, CmiGetSelfQueue()->size(), CmiTakeSelfQueue);
  took |= drainInto(pool, CmiGetQueue(CmiMyRank())->size(), CmiTakeThreadQueue);
  took |= drainInto(pool, (size_t)CsdNodeQueueLenGet(), CmiTakeNodePrioQueue);
  took |= drainInto(pool, (size_t)QueueSize(CpvAccess(CsdSchedQueue)),
                    CmiTakeThreadPrioQueue);
#if CMK_TASKQUEUE
  {
    TaskQueue tq = (TaskQueue)CpvAccess(CsdTaskQueue);
    taskq_idx n = tq->tail - tq->head;
    took |= drainInto(pool, n > 0 ? (size_t)n : 0, CmiTakeTaskQueue);
  }
#endif
  return took;
}

// Removes a uniformly random message from a non-empty pool.
void *drawFromPool(RandomizedState &st) {
  std::vector<void *> &pool = st.pool;
  std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
  size_t i = pick(st.rng);
  void *msg = pool[i];
  pool[i] = pool.back();
  pool.pop_back();
  return msg;
}

} // namespace

void CsdSchedulerRandomized() {
  RandomizedState &st = state();

  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);
    bool took = fillPool(st.pool);
    if (!st.pool.empty()) {
      void *msg = drawFromPool(st);
      CmiSchedulerReleaseIdle();
      CmiHandleMessage(msg);
    } else if (!took) {
      CmiSchedulerSetIdle();
    }
    CsdPeriodic();

  }
}

// Like CsdSchedulerRandomized, but returns once the pool and every source
// are empty instead of when the scheduler is stopped.
void CsdSchedulePollRandomized() {
  RandomizedState &st = state();

  while (1) {

    CcdRaiseCondition(CcdSCHEDLOOP);
    bool took = fillPool(st.pool);
    if (!st.pool.empty()) {
      void *msg = drawFromPool(st);
      CmiSchedulerReleaseIdle();
      CmiHandleMessage(msg);
    } else if (!took) {
      CmiSchedulerSetIdle();
      return;
    }
    CsdPeriodic();

  }
}
