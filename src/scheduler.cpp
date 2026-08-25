#include "scheduler.h"
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include "taskqueue.h"
#include <thread>
#include <cstdlib>
#include <vector>

CpvExtern(TaskQueue, CsdTaskQueue);

/* ---------------- Deliberate message reordering ---------------- */

/* A bounded random-reordering window over the per-PE ready queue.
 *
 * Message-driven code is meant to tolerate any order its messages can
 * legitimately arrive in, but on one machine over one transport that order is
 * nearly always the same -- so code that quietly depends on it passes every
 * local test and fails somewhere else. This makes the scheduler choose at
 * random among the next `depth` ready messages instead of taking them in
 * order.
 *
 * The window is bounded, and it is drained before the PE goes idle: this
 * reorders, it does not starve, and quiescence still means quiescence.
 */
CpvStaticDeclare(std::vector<void *> *, randomHoldBuf);
CpvStaticDeclare(unsigned int, randomHoldState);
static int randomQueueDepth = 0;  /* 0 = off */

/* Perturbing a bootstrap is not what this is for. Bringing a Charm++ program
   up is a fixed sequence of creations that later steps read back synchronously
   -- AMPI's ampiPeMgr group before the ampiParent elements that call
   ckLocalBranch() on it, the ampiParent element before the ampi element that
   calls ckLocal() on it -- and none of that is the concurrency a randomized
   queue is meant to explore. Reordering it does not find bugs, it only stops
   the program from starting.

   So the window is suspended by default and the layers above resume it when
   their own startup is finished: Charm++ at the end of _initDone, AMPI later
   still, once MPI_COMM_WORLD exists. Nested suspensions count, because they
   nest -- AMPI suspends before Charm++ resumes. A rescale suspends again at
   the cut and resumes on the far side of the restore, which is a bootstrap of
   the same kind. */
/* Per PE, not per process: in an SMP build every PE runs its own copy of the
   startup, and a shared counter would have one PE's resume cancel another
   PE's suspension. */
CpvStaticDeclare(int, randomQueueSuspends);

void CmiRandomizedQueueStats(long *taken, long *reordered);

void CmiRandomizedQueueSuspend(void) {
  if (CpvInitialized(randomQueueSuspends)) CpvAccess(randomQueueSuspends)++;
}
void CmiRandomizedQueueResume(void) {
  if (CpvInitialized(randomQueueSuspends) && CpvAccess(randomQueueSuspends) > 0)
    CpvAccess(randomQueueSuspends)--;
}
/* For the far side of a rescale: the restore is not paired with the
   suspensions taken before the cut, and re-running a proc-init on the way back
   takes fresh ones that nothing will ever pair with either. */
void CmiRandomizedQueueResumeAll(void) {
  /* This is the only caller-visible moment inside a run, and a rescale
     campaign never reaches the atexit report -- its processes are killed. Say
     where the counters stand, so a campaign log carries the evidence that the
     window was open through everything before this point. */
  if (randomQueueDepth > 0) {
    long taken = 0, reordered = 0;
    CmiRandomizedQueueStats(&taken, &reordered);
    /* stderr: this runs inside the rescale restore, where the path that
       forwards CmiPrintf output to the launcher is still being repaired. */
    fprintf(stderr,
            "Converse> Randomized message queue, PE %d: %ld of %ld scheduled "
            "messages out of FIFO order so far.\n",
            CmiMyPe(), reordered, taken);
    fflush(stderr);
  }
  if (CpvInitialized(randomQueueSuspends)) CpvAccess(randomQueueSuspends) = 0;
}

static inline bool randomHoldActive(void) {
  return randomQueueDepth > 0 && CpvInitialized(randomQueueSuspends) &&
         CpvAccess(randomQueueSuspends) == 0;
}

int CmiRandomizedQueueEnabled(void) { return randomHoldActive(); }

static inline bool randomHoldNonEmpty(void) {
  /* Not randomHoldActive(): a window filled while armed must still be drained
     after a disarm, or those messages are simply lost. */
  return randomQueueDepth > 0 && CpvInitialized(randomHoldBuf) &&
         CpvAccess(randomHoldBuf) != NULL && !CpvAccess(randomHoldBuf)->empty();
}

static inline unsigned int randomHoldNext(void) {
  /* xorshift32: per PE, no locking, good enough to shuffle with. */
  unsigned int x = CpvAccess(randomHoldState);
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  CpvAccess(randomHoldState) = x;
  return x;
}

/* How much reordering actually happened. A window only perturbs anything when
   two or more messages are in it at once, and how often that is true depends
   entirely on the application; without these counters "randomization is on" is
   not evidence that any order was changed. */
CpvStaticDeclare(long, randomHoldTaken);     /* messages drawn from the window */
CpvStaticDeclare(long, randomHoldReordered); /* ... that were not at its head */

/* Remove and return one message from the window, chosen at random. */
static void *randomHoldTakeOne(void) {
  std::vector<void *> &buf = *CpvAccess(randomHoldBuf);
  if (buf.empty()) return NULL;
  const size_t i = randomHoldNext() % buf.size();
  void *msg = buf[i];
  buf[i] = buf.back();
  buf.pop_back();
  CpvAccess(randomHoldTaken)++;
  if (i != 0) CpvAccess(randomHoldReordered)++;
  return msg;
}

/* Reported on the way out rather than at ConverseExit: the exit path a Charm++
   program takes does not reliably pass through there, and this number is worth
   nothing if it is sometimes missing. */
static void randomHoldReportAtExit(void) {
  if (randomQueueDepth <= 0) return;
  long taken = 0, reordered = 0;
  CmiRandomizedQueueStats(&taken, &reordered);
  fprintf(stderr,
          "Converse> Randomized message queue: %ld of %ld scheduled messages "
          "ran out of FIFO order on this process's first PE.\n",
          reordered, taken);
}

void CmiRandomizedQueueStats(long *taken, long *reordered) {
  *taken = CpvInitialized(randomHoldTaken) ? CpvAccess(randomHoldTaken) : 0;
  *reordered =
      CpvInitialized(randomHoldReordered) ? CpvAccess(randomHoldReordered) : 0;
}

void *CmiRandomizedQueueTake(void) {
  if (!randomHoldNonEmpty()) return NULL;
  return randomHoldTakeOne();
}

void CmiRandomizedQueueInit(char **argv) {
  CpvInitialize(std::vector<void *> *, randomHoldBuf);
  CpvInitialize(unsigned int, randomHoldState);
  CpvInitialize(long, randomHoldTaken);
  CpvInitialize(long, randomHoldReordered);
  CpvInitialize(int, randomQueueSuspends);
  /* Suspended until the layer above says its startup is done. A survivor
     coming back through here is about to run a restore, which wants the same
     treatment; CkRestoreGroupData drops it on the far side. */
  CpvAccess(randomQueueSuspends) = 1;
  /* A survivor comes back through here after a rescale, and its window may
     still hold messages that arrived before the cut. Keep the buffer. */
  if (CpvAccess(randomHoldBuf) == NULL)
    CpvAccess(randomHoldBuf) = new std::vector<void *>();

  int depth = 0;
  if (CmiGetArgIntDesc(argv, "+randomizedqueue", &depth,
                       "Run ready messages in a random order within a window "
                       "of this many, to shake out order-dependent bugs")) {
    if (depth < 2) depth = 2;
  } else {
    /* Not on the command line -- either it was never asked for, or this is a
       survivor's second pass and the first one already consumed the flag.
       Only the first case means "off". */
    depth = randomQueueDepth;
  }

  int seed = 0;
  if (!CmiGetArgIntDesc(argv, "+randomizedqueueseed", &seed,
                        "Seed for +randomizedqueue, to make a run repeatable"))
    seed = 12345;

  if (CmiMyRank() == 0) randomQueueDepth = depth;
  /* Per PE, so PEs make different choices rather than the same ones. A
     survivor whose PE number changed picks up a different stream, which is
     what we want: the point is variety, not reproducibility across rescales. */
  CpvAccess(randomHoldState) = (unsigned int)(seed + 7919 * (CmiMyPe() + 1));

  static bool announced = false;
  if (depth > 0 && CmiMyRank() == 0 && !announced) {
    static bool registered = false;
    if (!registered) { registered = true; atexit(randomHoldReportAtExit); }
  }
  if (depth > 0 && CmiMyPe() == 0 && !announced) {
    announced = true;
    CmiPrintf("Converse> Randomized message queue: window %d, seed %d. "
              "Message order is deliberately perturbed; timings from this run "
              "are not meaningful.\n", depth, seed);
  }
}

/* The next message to run from the per-PE queue, or NULL if there is none.
   With reordering off this is exactly "pop the head". */
static void *CsdNextLocalMessage(ConverseQueue<void *> *queue) {
  if (!randomHoldActive()) {
    /* Anything the window was still holding when it was suspended goes first,
       in the order the shuffle left it; after that this is a plain FIFO pop. */
    void *held = CmiRandomizedQueueTake();
    if (held != NULL) return held;
    if (queue->empty()) return NULL;
    return queue->pop().value();
  }

  std::vector<void *> &buf = *CpvAccess(randomHoldBuf);
  /* Fill the window from the queue, then choose within it. When the queue runs
     dry the window is drained rather than held, which is what keeps this from
     delaying anything indefinitely. */
  while ((int)buf.size() < randomQueueDepth && !queue->empty()) {
    auto r = queue->pop();
    if (!r) break;
    buf.push_back(r.value());
  }
  return randomHoldTakeOne();
}

/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  int loop_counter = 0;

  while (CmiStopFlag() == 0) {

    CcdRaiseCondition(CcdSCHEDLOOP);

    #ifdef CMK_USE_SHMEM
        CmiIpcBlock* block = CmiPopIpcBlock(CsvAccess(coreIpcManager_));
        if (block != nullptr) {
          CmiDeliverIpcBlockMsg(block);
        }
    #endif

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        void *msg = result.value();

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }

        // process event
        CmiHandleMessage(msg);
      }
    }

    // poll thread queue
    else if (!queue->empty() || randomHoldNonEmpty()) {
      void *msg = CsdNextLocalMessage(queue);
      if (msg == NULL) continue;

      // release idle if necessary
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }
        
      // process event
      CmiHandleMessage(msg);
    }

        // poll node prio queue
    else {
      // Try to acquire lock without blocking
      if (CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void* msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CmiUnlock(CsvAccess(CsdNodeQueueLock));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        
          // process event
          CmiHandleMessage(msg);
        } 
        else {
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          //empty queue so check thread prio queue
          if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }
        
          // process event
          CmiHandleMessage(msg);
        } else {
          #if CMK_TASKQUEUE
          // Check local task queue before going idle
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }
            // Found a task in our local queue
            CmiHandleMessage(task_msg);
          } else {
          #endif
            // the processor is idle
            // if not already idle, set idle and raise condition
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
          #if CMK_TASKQUEUE
          }
          #endif
        }
        }        
      } 
      else {
        // Could not acquire node queue lock, skip to thread prio queue
        if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } else {
          #if CMK_TASKQUEUE
          // Check local task queue before going idle
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }

            // Found a task in our local queue
            CmiHandleMessage(task_msg);

          } else {
          #endif
            // the processor is idle
            // if not already idle, set idle and raise condition
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
          #if CMK_TASKQUEUE
          }
          #endif
        }
      }
    }
    if((CmiMyRank() % backend_poll_thread == 0) && (loop_counter++ == (backend_poll_freq - 1)))
    {
      loop_counter = 0;
      comm_backend::progress();
    }

    CsdPeriodic();

  }
}

/**
 * Similar to CsdScheduler, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  // get pthread level queue
  ConverseQueue<void *> *queue = CmiGetQueue(CmiMyRank());

  // get node level queue
  ConverseNodeQueue<void *> *nodeQueue = CmiGetNodeQueue();

  while(1){

    CsdPeriodic();

    CcdRaiseCondition(CcdSCHEDLOOP);

    // poll node queue
    if (!nodeQueue->empty()) {
      auto result = nodeQueue->pop();
      if (result) {
        void *msg = result.value();

        // release idle if necessary
        if (CmiGetIdle()) {
          CmiSetIdle(false);
          CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
        }

        // process event
        CmiHandleMessage(msg);

      }
    }

    // poll thread queue
    else if (!queue->empty() || randomHoldNonEmpty()) {
      void *msg = CsdNextLocalMessage(queue);
      if (msg == NULL) continue;

      // release idle if necessary
      if (CmiGetIdle()) {
        CmiSetIdle(false);
        CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
      }

      // process event
      CmiHandleMessage(msg);

    }

    // poll node prio queue
    else {
      // Try to acquire lock without blocking
      if (CmiTryLock(CsvAccess(CsdNodeQueueLock)) == 0) {
        if (!QueueEmpty(CsvAccess(CsdNodeQueue))) {
          void *msg = QueueTop(CsvAccess(CsdNodeQueue));
          QueuePop(CsvAccess(CsdNodeQueue));
          CmiUnlock(CsvAccess(CsdNodeQueueLock));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } 
        else {
          CmiUnlock(CsvAccess(CsdNodeQueueLock));
          if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } 
        else {
          #if CMK_TASKQUEUE
          //because idle, check task queue
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
          
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }

            // Found a task in our local queue
            CmiHandleMessage(task_msg);

          }
          else
          {
          #endif
            comm_backend::progress();
            break; //break when queues are empty
          #if CMK_TASKQUEUE
          }
          #endif
        }
        }
      } 
      else {
        // Could not acquire node queue lock, skip to thread prio queue
        if (!QueueEmpty(CpvAccess(CsdSchedQueue))) {
          void *msg = QueueTop(CpvAccess(CsdSchedQueue));
          QueuePop(CpvAccess(CsdSchedQueue));

          // release idle if necessary
          if (CmiGetIdle()) {
            CmiSetIdle(false);
            CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
          }

          // process event
          CmiHandleMessage(msg);

        } 
        else {
          #if CMK_TASKQUEUE
          //because idle, check task queue
          void *task_msg = TaskQueuePopLocal();
          if (task_msg != NULL) {
            
            if (CmiGetIdle()) {
              CmiSetIdle(false);
              CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
            }

            // Found a task in our local queue
            CmiHandleMessage(task_msg);
            
          }
          else
          {
          #endif
            comm_backend::progress();
            break; //break when queues are empty
          #if CMK_TASKQUEUE
          }
          #endif
        }
      }
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

