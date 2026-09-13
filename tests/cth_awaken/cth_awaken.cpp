/* Tests for the pool-style thread hooks: custom awaken functions, the
 * BLOCKED/READY/RUNNING/TERMINATED state machine, post-switch actions
 * (CthSuspendBlocked, CthYield on a custom-awaken thread), keep-on-exit and
 * exit callbacks, and the standin exit path.
 *
 * Threads created here have an awaken function that pushes their token into
 * a process-wide queue; every PE runs a "poller" handler that pops one token
 * and hands it to CmiHandleMessage, so a thread resumes on whichever PE pops
 * it -- the pool model, without the scheduler table. Run with +pe 4. */
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

struct SharedQueue {
  std::mutex m;
  std::deque<void *> q;
  void push(void *x) { std::lock_guard<std::mutex> g(m); q.push_back(x); }
  void *pop() {
    std::lock_guard<std::mutex> g(m);
    if (q.empty()) return nullptr;
    void *x = q.front(); q.pop_front(); return x;
  }
};

static SharedQueue readyQ;     /* thread tokens (the "pool") */
static SharedQueue blockedL;   /* CthThread handles waiting for a wake-up */
static std::atomic<int> stopPolling{0};

struct Msg { char hdr[CmiMsgHeaderSizeBytes]; };
static int pollerIdx, wakerIdx, dualWakeIdx, exitIdx;

static void pushToken(CthThread t, void *arg) {
  ((SharedQueue *)arg)->push(CthGetToken(t));
}

/* --- completion event: the shim's mutex pattern (lock, block, unlock after
 * the switch) so a finisher can never race the waiter's transition --- */
struct Event {
  std::mutex m;
  int count = 0, target = 0;
  CthThread waiter = nullptr;
  static void unlockAfter(void *arg) { ((std::mutex *)arg)->unlock(); }
  void wait(int n) {
    m.lock();
    target = n;
    while (count < target) {
      waiter = CthSelf();
      CthSuspendBlocked(unlockAfter, &m); /* unlocks m once BLOCKED */
      m.lock();
    }
    count = 0; target = 0; waiter = nullptr;
    m.unlock();
  }
  void signal() {
    CthThread w = nullptr;
    m.lock();
    ++count;
    if (target && count >= target && waiter) { w = waiter; waiter = nullptr; }
    m.unlock();
    if (w && !CthAwakenIfBlocked(w))
      CmiAbort("Event: waiter registered but not BLOCKED\n");
  }
};
static Event done;

/* --- handlers --- */
static void pollerHandler(void *m) {
  if (stopPolling.load()) { CmiFree(m); return; }
  void *tok = readyQ.pop();
  if (tok) CmiHandleMessage(tok);
  CmiPushPE(CmiMyRank(), m); /* keep polling */
}
static void wakerHandler(void *m) {
  if (stopPolling.load()) { CmiFree(m); return; }
  CthThread t = (CthThread)blockedL.pop();
  if (t && !CthAwakenIfBlocked(t))
    CmiAbort("waker: thread on the blocked list was not BLOCKED\n");
  CmiPushPE(CmiMyRank(), m);
}
static void sendLoop(int rank, int idx) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  CmiInitMsgHeader(m, (int)sizeof(Msg));
  CmiSetHandler(m, idx);
  CmiPushPE(rank, m);
}

/* --- phase A: block/wake across PEs, resume where popped --- */
static std::atomic<int> resumedOn[64];
static void registerBlocked(void *self) { blockedL.push(self); }
static void phaseAThread(void *) {
  for (int r = 0; r < 4; r++) {
    resumedOn[CmiMyPe()]++;
    if (CthGetState(CthSelf()) != CTH_STATE_RUNNING) CmiAbort("A: not RUNNING while running\n");
    CthSuspendBlocked(registerBlocked, CthSelf());
  }
  done.signal();
}

/* --- phase B: yield stress; segments of one thread must be sequential --- */
struct YieldT { std::atomic<int> seg{-1}; };
static void phaseBThread(void *arg) {
  YieldT *y = (YieldT *)arg;
  for (int i = 0; i < 2000; i++) {
    int prev = y->seg.exchange(i);
    if (prev != i - 1) CmiAbort("B: segment order broken (double resume?)\n");
    CthYield();
  }
  done.signal();
}

/* --- phase C: two PEs race to awaken one blocked thread --- */
static std::atomic<CthThread> victim{nullptr};
static std::atomic<int> wakeReturns{0}, victimRuns{0};
static void setVictim(void *self) { victim.store((CthThread)self); }
static void phaseCThread(void *) {
  victimRuns++;
  CthSuspendBlocked(setVictim, CthSelf());
  victimRuns++;
  done.signal();
}
static void dualWakeHandler(void *m) {
  CmiFree(m);
  CthThread v;
  while ((v = victim.load()) == nullptr) {}
  wakeReturns += CthAwakenIfBlocked(v);
  done.signal();
}

/* --- phase D: exit callbacks, keep-on-exit --- */
static std::atomic<int> exitCalls{0}, exitStateOk{0};
static CthThread namedT;
static void namedExit(void *arg) {
  exitCalls++;
  if (CthGetState((CthThread)arg) == CTH_STATE_TERMINATED) exitStateOk++;
  done.signal();
}
static void detachedExit(void *) { exitCalls++; done.signal(); }
static void trivialThread(void *) {}

static void exitHandler(void *m) { CmiFree(m); CsdExitScheduler(); }

static CthThread spawn(CthVoidFn fn, void *arg) {
  CthThread t = CthCreate(fn, arg, 65536);
  CthSetAwakenFn(t, pushToken, &readyQ);
  return t;
}

static void driver(void *) {
  int npes = CmiNumPes();

  /* A */
  const int NA = 32;
  for (int i = 0; i < NA; i++) CthAwakenIfBlocked(spawn(phaseAThread, nullptr));
  done.wait(NA);
  int total = 0, pesUsed = 0;
  for (int p = 0; p < npes; p++) { total += resumedOn[p]; pesUsed += resumedOn[p] > 0; }
  if (total != NA * 4) CmiAbort("A: wrong resume count\n");
  if (npes > 1 && pesUsed < 2) CmiAbort("A: threads never resumed on a second PE\n");
  CmiPrintf("A ok: %d resumes over %d PEs\n", total, pesUsed);

  /* B */
  const int NB = 16;
  std::vector<YieldT> ys(NB);
  for (int i = 0; i < NB; i++) CthAwakenIfBlocked(spawn(phaseBThread, &ys[i]));
  done.wait(NB);
  CmiPrintf("B ok: %d threads x 2000 yields across PEs\n", NB);

  /* C */
  CthAwakenIfBlocked(spawn(phaseCThread, nullptr));
  int nwakers = CmiMyNodeSize() >= 3 ? 2 : 1;
  for (int r = 1; r <= nwakers; r++) sendLoop(r, dualWakeIdx);
  done.wait(1 + nwakers);
  if (wakeReturns.load() != 1) CmiAbort("C: concurrent CthAwakenIfBlocked did not resolve to exactly one\n");
  if (victimRuns.load() != 2) CmiAbort("C: victim ran wrong number of times\n");
  CmiPrintf("C ok: exactly one of %d concurrent wakes won\n", nwakers);

  /* D */
  namedT = spawn(trivialThread, nullptr);
  CthSetKeepOnExit(namedT, 1);
  CthSetExitFn(namedT, namedExit, namedT);
  CthAwakenIfBlocked(namedT);
  done.wait(1);
  if (CthGetState(namedT) != CTH_STATE_TERMINATED) CmiAbort("D: named thread not TERMINATED\n");
  CthFree(namedT);
  CthThread d = spawn(trivialThread, nullptr);
  CthSetExitFn(d, detachedExit, nullptr);
  CthAwakenIfBlocked(d);
  done.wait(1);
  if (exitCalls.load() != 2 || exitStateOk.load() != 1) CmiAbort("D: exit callbacks wrong\n");
  CmiPrintf("D ok: exit callbacks fired, named thread freed after join\n");

  /* stop pollers/wakers, then exit; ranks > 0 are parked on standins (F) */
  stopPolling.store(1);
  for (int r = 0; r < CmiMyNodeSize(); r++) {
    Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
    CmiInitMsgHeader(m, (int)sizeof(Msg));
    CmiSetHandler(m, exitIdx);
    CmiPushPE(r, m);
  }
}

static void startfn(int argc, char **argv) {
  pollerIdx = CmiRegisterHandler((CmiHandler)pollerHandler);
  wakerIdx = CmiRegisterHandler((CmiHandler)wakerHandler);
  dualWakeIdx = CmiRegisterHandler((CmiHandler)dualWakeHandler);
  exitIdx = CmiRegisterHandler((CmiHandler)exitHandler);
  /* every PE polls the shared token queue; ranks > 0 also wake blocked threads */
  sendLoop(CmiMyRank(), pollerIdx);
  if (CmiMyRank() > 0) sendLoop(CmiMyRank(), wakerIdx);
  if (CmiMyRank() == 0) {
    CthThread drv = spawn(driver, nullptr);
    CthAwakenIfBlocked(drv);
    return; /* main thread becomes this PE's scheduler */
  }
  /* F: park this PE's main thread; a standin runs the scheduler. The exit
   * message must bring the main thread back so the PE exits normally. */
  CthSuspend();
  CmiPrintf("F: PE %d main thread resumed after scheduler stop\n", CmiMyPe());
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ConverseInit(argc, argv, startfn);
  return 0;
}
