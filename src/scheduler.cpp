#include "scheduler.h"
#if CMK_TASKQUEUE
#include "taskqueue.h"
CpvExtern(TaskQueue, CsdTaskQueue);
#endif

static inline void releaseIdle() {
  if (CmiGetIdle()) {
    CmiSetIdle(false);
    CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
  }
}

/* Public: a poll entry outside the runtime that found work must call this
 * before handling it, exactly as the built-in queue pollers do. */
void CsdReleaseIdle(void) { releaseIdle(); }

/* maySleep: only the blocking scheduler loop parks the PE; CsdSchedulePoll
 * must return promptly */
static inline void setIdle(bool maySleep) {
  if (!CmiGetIdle()) {
    CmiSetIdle(true);
    CmiSetIdleTime(CmiWallTimer());
    CsdIdleReset();
    CcdRaiseCondition(CcdPROCESSOR_BEGIN_IDLE);
  }
  // if already idle, call still idle and (maybe) long idle
  else {
    CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
    if (CmiWallTimer() - CmiGetIdleTime() > 10.0) {
      CcdRaiseCondition(CcdPROCESSOR_LONG_IDLE);
    }
    if (maySleep) CsdIdleSleepMaybe();
  }
}

//poll converse-level node queue
static int pollConverseNodeQueue(void *) {
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();
  if (!nodeQueue->empty()) {
    auto result = nodeQueue->pop();
    if (result) {
      void *msg = result.value();
      releaseIdle();
      // process event
      CmiHandleMessage(msg);
      return 1;
    }
  }
  return 0;
}

//poll this PE's self queue
static int pollSelfQueue(void *) {
  ConverseSelfQueue<void *> *selfQueue = CmiGetSelfQueue();
  if (!selfQueue->empty()) {
    void *msg = selfQueue->pop();
    releaseIdle();
    // process event
    CmiHandleMessage(msg);
    return 1;
  }
  return 0;
}

//poll converse-level thread queue
static int pollConverseThreadQueue(void *) {
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());
  if (!queue->empty()) {
    // get next event (guaranteed to be there because only single consumer)
    void *msg = queue->pop().value();
    releaseIdle();
    // process event
    CmiHandleMessage(msg);
    return 1;
  }
  return 0;
}

//poll node priority queue
static int pollNodePrioQueue(void *) {
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
      releaseIdle();
      // process event
      CmiHandleMessage(msg);
      return 1;
    } else {
      CmiUnlock(CsvAccess(CsdNodeQueueLock));
    }
  }
  return 0;
}

//poll thread priority queue
static int pollThreadPrioQueue(void *) {
  if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
    void *msg = QueueTop(CpvAccess(CsdSchedQueue));
    QueuePop(CpvAccess(CsdSchedQueue));
    releaseIdle();
    // process event
    CmiHandleMessage(msg);
    return 1;
  }
  return 0;
}

static int pollProgress(void *)
{
  if(CmiMyRank() % backend_poll_thread == 0) comm_backend::progress();
  return 0; //polling progress doesn't count
}

#if CMK_TASKQUEUE
static int pollTaskQueue(void *) {
  void *task_msg = TaskQueuePopLocal();
  if (task_msg != nullptr) {
    releaseIdle();
    CmiHandleMessage(task_msg);
    return 1;
  }
  return 0;
}
#endif

/* The runtime's own queues, always the leading entries of every table so
 * Converse messaging keeps working whatever else a PE polls. Relative
 * frequencies as in PR #150; the comm-progress weight is +backend_poll_freq
 * (default 4). */
int CsdBuiltinPollEntries(CsdPollEntry *out, int max) {
  int n = 0;
  auto add = [&](CsdPollFn fn, unsigned freq, const char *name) {
    if (n < max) out[n++] = CsdPollEntry{fn, nullptr, freq, name};
  };
  /* The node queue carries nodegroup and node-level traffic; equal weight
   * with the PE queues (was 1 in #150). NOTE: Charm++'s pingpong shows
   * NodeGroup messages ~0.2 us (1.5x) slower with the table sweep than with
   * main's if/else scheduler, and that bisects to the table itself
   * (880f57c), not to this weight -- weight 16 vs 1 measured the same, and
   * so did polling the node queue first every iteration. Unresolved;
   * suspected cost of the per-sweep empty() checks on the shared
   * multi-consumer queue. See argobots-succession/charm-gate-report.md. */
  add(pollConverseNodeQueue, 16, "node queue");
  add(pollSelfQueue, 16, "self queue");
  add(pollConverseThreadQueue, 16, "PE queue");
  add(pollNodePrioQueue, 1, "node prio queue");
  add(pollThreadPrioQueue, 16, "PE prio queue");
  add(pollProgress, (unsigned)backend_poll_freq, "comm progress");
#if CMK_TASKQUEUE
  add(pollTaskQueue, 1, "task queue");
#endif
  return n;
}

/* one sweep: forward from the rotating base, stop at the first slot that
 * did work; re-read the table every slot because a handler may have
 * installed a new one (consumed at the loop top, but the pointer is what
 * the next iteration must use) */
static inline bool CsdSweep(uint64_t base) {
  for (unsigned t = 0; t < CSD_TABLE_SLOTS; ++t) {
    unsigned idx = static_cast<unsigned>((base + t) & (CSD_TABLE_SLOTS - 1));
    CsdSchedTableStruct *tab = CpvAccess(CsdPollTable);
    if (tab->slotFn[idx](tab->slotCtx[idx])) {
      int o = tab->owner[idx];
      if (o >= 0) tab->counts[o]++;
      return true;
    }
  }
  return false;
}

/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {

  uint64_t loop_counter = 0;
  CpvAccess(CsdSchedDepth)++;

  while (CmiStopFlag() == 0) {

    CsdSchedTableLoopTop();
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
    if (!CsdSweep(loop_counter)) {
      setIdle(true);
    }
    CsdPeriodic();
    loop_counter++;

  }
  CpvAccess(CsdSchedDepth)--;
}

/**
 * Similar to CsdScheduler, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  uint64_t loop_counter = 0;
  CpvAccess(CsdSchedDepth)++;

  while(1){

    CsdSchedTableLoopTop();
    CcdRaiseCondition(CcdSCHEDLOOP);
    //poll queues: sweep the full table before concluding it's empty, so
    //a message doesn't have to wait for loop_counter to rotate back
    //around to its slot
    if (!CsdSweep(loop_counter)) {
      //swept the whole table and every slot was empty: done
      setIdle(false);
      CpvAccess(CsdSchedDepth)--;
      return;
    }
    CsdPeriodic();
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
          // FIFO strategies go to the back of their priority level, LIFO
          // strategies to the front. Bitvector priorities (BFIFO/BLIFO) are
          // not supported yet and are queued at priority 0.
          switch (strategy){
            case CQS_QUEUEING_FIFO:
              QueuePush(q, Message, 0);
              break;
            case CQS_QUEUEING_LIFO:
              QueuePushFront(q, Message, 0);
              break;
            case CQS_QUEUEING_IFIFO:
              QueuePush(q, Message, (int)prioptr[0]);
              break;
            case CQS_QUEUEING_ILIFO:
              QueuePushFront(q, Message, (int)prioptr[0]);
              break;
            case CQS_QUEUEING_LFIFO:
              QueuePush(q, Message, ((long long*)prioptr)[0]);
              break;
            case CQS_QUEUEING_LLIFO:
              QueuePushFront(q, Message, ((long long*)prioptr)[0]);
              break;
            default:
              // unknown or unsupported strategy, default to FIFO
              QueuePush(q, Message, 0);
              break;
          }
}

//network progress
void CmiNetworkProgress(){
  comm_backend::progress();
}

void CmiMachineProgressImpl() { CmiNetworkProgress(); }
