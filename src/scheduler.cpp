// Shared scheduler entry points.
//
// The loops themselves live in the two implementation files:
//   scheduler_registered.cpp  queue registration + slot table (default)
//   scheduler_old.cpp         original hardcoded chain (+old-scheduler)
// This file picks between them and holds the pieces both share.

#include "scheduler.h"

// false => registered scheduler (the default). Written once by
// CmiSchedulerInitArgs() on the main thread, before CmiStartThreads() spawns
// any PE, and only read from then on, so no synchronization is needed.
bool _Cmi_useOldScheduler = false;

void CmiSchedulerInitArgs(char **argv) {
  _Cmi_useOldScheduler = CmiGetArgFlagDesc(
      argv, "+old-scheduler",
      "Use the original hardcoded scheduler loop instead of the default "
      "registered-queue scheduler");
}

// Idle bookkeeping, shared so both implementations raise the same conditions.
void CmiSchedulerReleaseIdle() {
  if (CmiGetIdle()) {
    CmiSetIdle(false);
    CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
  }
}

void CmiSchedulerSetIdle() {
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

// Messages from peer processes on this host arrive in the shared-memory pool
// rather than through the network backend, so every loop has to drain it --
// including CsdSchedulePoll, or a program that waits inside it would never
// see a message a peer put in the pool.
bool CmiSchedulerPollIpc() {
  CmiIpcManager *manager = CsvAccess(coreIpcManager_);
  if (manager == nullptr) return false;
  CmiIpcBlock *block = CmiPopIpcBlock(manager);
  if (block == nullptr) return false;
  CmiDeliverIpcBlockMsg(block);
  return true;
}

/**
 * The main scheduler loop for the Charm++ runtime.
 */
void CsdScheduler() {
  if (CmiSchedulerIsOld()) CsdSchedulerOld();
  else CsdSchedulerRegistered();
}

/**
 * Similar to CsdScheduler, but return when the queues
 * are empty, not when the scheduler is stopped.
 */
void CsdSchedulePoll() {
  if (CmiSchedulerIsOld()) CsdSchedulePollOld();
  else CsdSchedulePollRegistered();
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
