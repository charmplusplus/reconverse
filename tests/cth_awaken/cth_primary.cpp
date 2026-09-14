/* The primary-thread model the Argobots shim uses: ConverseInit returns to
 * main on rank 0 (usched=1, initret=1); ranks > 0 run the scheduler from
 * startfn. Rank 0's main thread gets a custom awaken function but must stay
 * pinned to PE 0: another PE wakes it repeatedly and it must resume on PE 0
 * every time. Run with +pe 4. */
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

struct SharedQueue {
  std::mutex m; std::deque<void *> q;
  void push(void *x) { std::lock_guard<std::mutex> g(m); q.push_back(x); }
  void *pop() { std::lock_guard<std::mutex> g(m); if (q.empty()) return nullptr; void *x = q.front(); q.pop_front(); return x; }
};
static SharedQueue wrongQ; /* a queue nobody polls */
static SharedQueue blockedL;
static std::atomic<int> stopPolling{0};
struct Msg { char hdr[CmiMsgHeaderSizeBytes]; };
static int wakerIdx, exitIdx, checkIdx;

/* the contract for a PE main thread: the custom function is called (so a
 * library can keep queue order) but must deliver the token to the home PE */
static void pushToken(CthThread t, void *arg) {
  if (CthIsPeMainThread(t)) { CmiPushPE(CthGetHomeRank(t), CthGetToken(t)); return; }
  ((SharedQueue *)arg)->push(CthGetToken(t));
}
static void registerBlocked(void *self) { blockedL.push(self); }

static void wakerHandler(void *m) {
  if (stopPolling.load()) { CmiFree(m); return; }
  CthThread t = (CthThread)blockedL.pop();
  if (t && !CthAwakenIfBlocked(t)) CmiAbort("waker: not BLOCKED\n");
  CmiPushPE(CmiMyRank(), m);
}
static void checkHandler(void *m) {
  CmiFree(m);
  if (wrongQ.pop() != nullptr) CmiAbort("a token was pushed to the unpolled queue\n");
}
static void exitHandler(void *m) { CmiFree(m); CsdExitScheduler(); }

static void registerHandlers() {
  wakerIdx = CmiRegisterHandler((CmiHandler)wakerHandler);
  checkIdx = CmiRegisterHandler((CmiHandler)checkHandler);
  exitIdx = CmiRegisterHandler((CmiHandler)exitHandler);
}
static void startfn(int, char **) { /* ranks > 0 only (rank 0 returns from ConverseInit) */
  registerHandlers();
  CsdScheduler(-1);
}
static void send(int rank, int idx) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  CmiInitMsgHeader(m, (int)sizeof(Msg));
  CmiSetHandler(m, idx);
  CmiPushPE(rank, m);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ConverseInit(argc, argv, startfn, 1, 1);
  /* rank 0, primary thread */
  registerHandlers();
  if (CmiMyNodeSize() < 2) { CmiPrintf("needs +pe >= 2\n"); ConverseExit(1); }
  CthThread me = CthSelf();
  CthSetAwakenFn(me, pushToken, &wrongQ);
  if (CthGetState(me) != CTH_STATE_RUNNING) CmiAbort("primary not RUNNING\n");
  for (int r = 1; r < CmiMyNodeSize(); r++) send(r, wakerIdx);
  for (int i = 0; i < 200; i++) {
    CthSuspendBlocked(registerBlocked, me);
    if (CmiMyPe() != 0) CmiAbort("primary resumed off PE 0\n");
    if (CthGetState(me) != CTH_STATE_RUNNING) CmiAbort("primary not RUNNING after resume\n");
  }
  send(0, checkIdx); /* handled by PE 0's scheduler next time we block */
  CthSuspendBlocked(registerBlocked, me);
  CmiPrintf("primary ok: 201 cross-PE wakes, all resumed on PE 0\n");
  stopPolling.store(1);
  for (int r = 1; r < CmiMyNodeSize(); r++) send(r, exitIdx);
  ConverseExit(0);
  return 0;
}
