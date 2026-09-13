/* Library mode: ConverseInit returns to main on rank 0 (usched=1, initret=1),
 * ranks > 0 run the scheduler from startfn, main does some work, then
 * ConverseFinalize() stops the workers, joins them and RETURNS, so the
 * process ends by main returning 0, not by exit() inside the runtime.
 * Run with +pe 4. */
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

struct Msg { char hdr[CmiMsgHeaderSizeBytes]; int from; };
static int pingIdx, pongIdx;
static std::atomic<int> pongs{0};
static std::atomic<int> pongMask{0};

static void send(int rank, int idx) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  CmiInitMsgHeader(m, (int)sizeof(Msg));
  CmiSetHandler(m, idx);
  m->from = CmiMyPe();
  CmiPushPE(rank, m);
}
static void pingHandler(void *vm) { CmiFree(vm); send(0, pongIdx); }
static void pongHandler(void *vm) { Msg *m = (Msg *)vm; pongMask |= 1 << m->from; CmiFree(vm); pongs++; }
static void registerHandlers() {
  pingIdx = CmiRegisterHandler((CmiHandler)pingHandler);
  pongIdx = CmiRegisterHandler((CmiHandler)pongHandler);
}
static void startfn(int, char **) { registerHandlers(); CsdScheduler(-1); }

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ConverseSetLibraryMode(1);
  ConverseInit(argc, argv, startfn, 1, 1);
  registerHandlers();
  int n = CmiMyNodeSize();
  for (int round = 0; round < 50; round++) {
    for (int r = 1; r < n; r++) send(r, pingIdx);
    double t0 = CmiWallTimer();
    while (pongs.load() < (round + 1) * (n - 1)) {
      CsdSchedulePoll();
      if (CmiWallTimer() - t0 > 5.0) { printf("pongs never arrived\n"); return 2; }
    }
  }
  if (pongMask.load() != ((1 << n) - 2)) { printf("not every rank answered\n"); return 3; }
  printf("work done: %d pongs from %d workers\n", pongs.load(), n - 1);
  ConverseFinalize();
  printf("finalized cleanly, main returning\n");
  return 0;
}
