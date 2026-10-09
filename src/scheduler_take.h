#ifndef _SCHEDULER_TAKE_H_
#define _SCHEDULER_TAKE_H_
// "Take" forms of the scheduler's message sources.
//
// Each function removes one message from its source and returns it, or
// returns nullptr when the source has nothing to give. They do not run the
// message and do not touch idle state. The registered scheduler's poll
// handlers are take + CmiHandleMessage; the randomized scheduler
// (+randomized_msgq) drains them into its pool instead.
//
// They are inline so a poll handler compiles to the same code as when the
// take was written out in its body.

#include "scheduler.h"
#if CMK_TASKQUEUE
#include "taskqueue.h"
CpvExtern(TaskQueue, CsdTaskQueue);
#endif

// converse-level node queue
inline void *CmiTakeNodeQueue() {
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();
  if (!nodeQueue->empty()) {
    auto result = nodeQueue->pop();
    if (result) return result.value();
  }
  return nullptr;
}

// this PE's self queue
inline void *CmiTakeSelfQueue() {
  ConverseSelfQueue<void *> *selfQueue = CmiGetSelfQueue();
  if (!selfQueue->empty()) return selfQueue->pop();
  return nullptr;
}

// converse-level thread queue
inline void *CmiTakeThreadQueue() {
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());
  if (!queue->empty()) {
    // guaranteed to be there because this PE is the only consumer
    return queue->pop().value();
  }
  return nullptr;
}

// node priority queue (CsdNodeQueue)
inline void *CmiTakeNodePrioQueue() {
  // Check the queue length before reaching for the lock. CmiTryLock is a
  // CAS on a single process-wide cacheline, and an idle PE would otherwise
  // execute it on every loop iteration; with many PEs per process that one
  // line dominates the scheduler loop and so the latency of noticing any
  // message at all. Measured at 1 process x 120 PEs: 4201 ns per idle
  // iteration before, 88 ns after.
  if (CsdNodeQueueLenGet() > 0 &&
      CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
    if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
      void *msg = QueueTop(CsvAccess(CsdNodeQueue));
      QueuePop(CsvAccess(CsdNodeQueue));
      CsdNodeQueueLenAdd(-1);
      CmiUnlock(CsvAccess(CsdNodeQueueLock));
      return msg;
    }
    CmiUnlock(CsvAccess(CsdNodeQueueLock));
  }
  return nullptr;
}

// this PE's thread priority queue (CsdSchedQueue)
inline void *CmiTakeThreadPrioQueue() {
  if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
    void *msg = QueueTop(CpvAccess(CsdSchedQueue));
    QueuePop(CpvAccess(CsdSchedQueue));
    return msg;
  }
  return nullptr;
}

#if CMK_TASKQUEUE
// this PE's work-stealing task queue
inline void *CmiTakeTaskQueue() { return TaskQueuePopLocal(); }
#endif

#endif
