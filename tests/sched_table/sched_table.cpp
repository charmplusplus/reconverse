/* Per-PE scheduler table: slot allocation, installing a table with a
 * user poll entry on some PEs (threads then resume only there), installing
 * from inside a handler (nested; the replaced table must be retired, not
 * freed under the sweep), superseding a pending install, and the
 * idle-release contract for user poll entries. Run with +pe 4. */
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

struct SharedQueue {
  std::mutex m; std::deque<void *> q;
  void push(void *x) { std::lock_guard<std::mutex> g(m); q.push_back(x); }
  void *pop() { std::lock_guard<std::mutex> g(m); if (q.empty()) return nullptr; void *x = q.front(); q.pop_front(); return x; }
};
static SharedQueue poolQ, blockedL;
static std::atomic<int> stopWaker{0};
struct Msg { char hdr[CmiMsgHeaderSizeBytes]; };
static int wakerIdx, nestedIdx, exitIdx;
static std::atomic<int> resumedOn[64];
static std::atomic<int> endIdleOn[64];

/* the user poll entry: pop one token from the pool and run it */
static int pollPool(void *ctx) {
  void *tok = ((SharedQueue *)ctx)->pop();
  if (!tok) return 0;
  CsdReleaseIdle();
  CmiHandleMessage(tok);
  return 1;
}
static void pushToken(CthThread t, void *arg) { ((SharedQueue *)arg)->push(CthGetToken(t)); }
static void awakenToPE0(CthThread t, void *) { CmiPushPE(0, CthGetToken(t)); }
static void registerBlocked(void *self) { blockedL.push(self); }

struct Event {
  std::mutex m; int count = 0, target = 0; CthThread waiter = nullptr;
  static void unlockAfter(void *arg) { ((std::mutex *)arg)->unlock(); }
  void wait(int n) {
    m.lock(); target = n;
    while (count < target) { waiter = CthSelf(); CthSuspendBlocked(unlockAfter, &m); m.lock(); }
    count = 0; target = 0; waiter = nullptr; m.unlock();
  }
  void signal() {
    CthThread w = nullptr;
    m.lock(); ++count;
    if (target && count >= target && waiter) { w = waiter; waiter = nullptr; }
    m.unlock();
    if (w && !CthAwakenIfBlocked(w)) CmiAbort("Event: waiter not BLOCKED\n");
  }
};
static Event done;

static void wakerHandler(void *m) {
  if (stopWaker.load()) { CmiFree(m); return; }
  CthThread t = (CthThread)blockedL.pop();
  if (t && !CthAwakenIfBlocked(t)) CmiAbort("waker: not BLOCKED\n");
  CmiPushPE(CmiMyRank(), m);
}
static void send(int rank, int idx) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  CmiInitMsgHeader(m, (int)sizeof(Msg));
  CmiSetHandler(m, idx);
  CmiPushPE(rank, m);
}
static CsdSchedTable poolTable(unsigned freq) {
  CsdPollEntry e{pollPool, &poolQ, freq, "test pool"};
  return CsdSchedTableCreate(&e, 1);
}
/* runs on PE 2 inside a sweep: replace PE 2's own table twice; the first
 * pending install is superseded, the current table is retired, not freed */
static void nestedHandler(void *m) {
  CmiFree(m);
  CsdSchedTableInstall(CmiMyRank(), poolTable(8));
  CsdSchedTableInstall(CmiMyRank(), poolTable(32));
  done.signal();
}
static void exitHandler(void *m) { CmiFree(m); CsdExitScheduler(); }
static void endIdleCb(void *) { endIdleOn[CmiMyPe()]++; }

static void worker(void *) {
  for (int r = 0; r < 3; r++) {
    resumedOn[CmiMyPe()]++;
    CthSuspendBlocked(registerBlocked, CthSelf());
  }
  resumedOn[CmiMyPe()]++;
  done.signal();
}
static void oneShot(void *) { resumedOn[CmiMyPe()]++; done.signal(); }

static CthThread spawn(CthVoidFn fn) {
  CthThread t = CthCreate(fn, nullptr, 65536);
  CthSetAwakenFn(t, pushToken, &poolQ);
  return t;
}
static void spinWait(double sec) { double t0 = CmiWallTimer(); while (CmiWallTimer() - t0 < sec) {} }

static void driver(void *) {
  int n = CmiMyNodeSize();
  if (n < 3) CmiAbort("needs +pe >= 3\n");

  /* 1: slot allocation */
  CsdPollEntry es[3] = {{pollPool, &poolQ, 1, "light"}, {pollPool, &poolQ, 3, "mid"}, {pollPool, &poolQ, 60, "heavy"}};
  CsdSchedTable t = CsdSchedTableCreate(es, 3);
  int s0 = CsdSchedTableSlots(t, 0), s1 = CsdSchedTableSlots(t, 1), s2 = CsdSchedTableSlots(t, 2);
  if (s0 < 1 || s1 < 1 || s2 < 1) CmiAbort("1: an entry got no slot\n");
  if (!(s2 > s1 && s1 >= s0)) CmiAbort("1: slot counts not ordered by frequency\n");
  if (s0 + s1 + s2 > 64 - CsdSchedTableNumBuiltin(t)) CmiAbort("1: user slots exceed the budget\n");
  if (CsdSchedTableSlots(t, 3) != -1) CmiAbort("1: out-of-range entry not rejected\n");
  CsdSchedTableDestroy(t);
  CmiPrintf("1 ok: slots light=%d mid=%d heavy=%d, builtin entries=%d\n", s0, s1, s2, CsdSchedTableNumBuiltin(poolTable(1)));

  /* 2: install the pool on ranks 1..n-1; PE 0 keeps the default table */
  for (int r = 1; r < n; r++) CsdSchedTableInstall(r, poolTable(16));
  send(0, wakerIdx);
  const int NW = 32;
  for (int i = 0; i < NW; i++) CthAwakenIfBlocked(spawn(worker));
  done.wait(NW);
  int total = 0, used = 0;
  for (int p = 0; p < n; p++) { total += resumedOn[p]; used += resumedOn[p] > 0; }
  if (resumedOn[0] != 0) CmiAbort("2: a pool thread ran on PE 0, which does not poll the pool\n");
  if (total != NW * 4) CmiAbort("2: resume count wrong\n");
  if (used < 2) CmiAbort("2: pool polled by only one PE\n");
  CmiPrintf("2 ok: %d resumes on %d PEs, none on PE 0\n", total, used);

  /* 3: nested install on PE 2 from inside a handler, then keep using the pool */
  send(2, nestedIdx);
  done.wait(1);
  for (int p = 0; p < n; p++) resumedOn[p] = 0;
  for (int i = 0; i < NW; i++) CthAwakenIfBlocked(spawn(worker));
  done.wait(NW);
  if (resumedOn[0] != 0) CmiAbort("3: pool thread on PE 0\n");
  CmiPrintf("3 ok: PE 2 replaced its table from inside a handler; %d resumes after\n", NW * 4);

  /* 4: idle release: let the pollers go idle, then hand them pool work */
  spinWait(0.2);
  int before = 0; for (int p = 1; p < n; p++) before += endIdleOn[p];
  CthAwakenIfBlocked(spawn(oneShot));
  done.wait(1);
  int after = 0; for (int p = 1; p < n; p++) after += endIdleOn[p];
  if (after <= before) CmiAbort("4: pool work on an idle PE did not raise END_IDLE\n");
  CmiPrintf("4 ok: END_IDLE raised for pool work (%d -> %d)\n", before, after);

  stopWaker.store(1);
  for (int r = 0; r < n; r++) send(r, exitIdx);
}

static void startfn(int, char **) {
  wakerIdx = CmiRegisterHandler((CmiHandler)wakerHandler);
  nestedIdx = CmiRegisterHandler((CmiHandler)nestedHandler);
  exitIdx = CmiRegisterHandler((CmiHandler)exitHandler);
  CcdCallOnConditionKeep(CcdPROCESSOR_END_IDLE, endIdleCb, nullptr);
  if (CmiMyRank() == 0) {
    CthThread d = CthCreate(driver, nullptr, 262144);
    CthSetAwakenFn(d, awakenToPE0, nullptr);
    CthAwakenIfBlocked(d);
  }
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ConverseInit(argc, argv, startfn);
  return 0;
}
