/* Sleep on idle: with +CmiSleepOnIdle the idle PEs must stop burning CPU,
 * and a message to a sleeping PE must still be handled promptly (the push
 * notifies it). Without the flag the same program runs, spinning. +pe 4. */
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

struct Msg { char hdr[CmiMsgHeaderSizeBytes]; double sent; };
static int pingIdx, pongIdx, exitIdx;
static std::atomic<int> pongs{0};
static std::atomic<double> worstRtt{0.0};

static double cpuSeconds() {
  struct rusage ru; getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
}
static void send(int rank, int idx, double sent) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  CmiInitMsgHeader(m, (int)sizeof(Msg));
  CmiSetHandler(m, idx);
  m->sent = sent;
  CmiPushPE(rank, m);
}
static void pingHandler(void *vm) { Msg *m = (Msg *)vm; double s = m->sent; CmiFree(m); send(0, pongIdx, s); }
static void pongHandler(void *vm) {
  Msg *m = (Msg *)vm;
  double rtt = CmiWallTimer() - m->sent; CmiFree(m);
  double w = worstRtt.load(); while (rtt > w && !worstRtt.compare_exchange_weak(w, rtt)) {}
  pongs++;
}
static void exitHandler(void *m) { CmiFree(m); CsdExitScheduler(); }
static void spinWait(double sec) { double t0 = CmiWallTimer(); while (CmiWallTimer() - t0 < sec) {} }

static void driver(void *) {
  int n = CmiMyNodeSize();
  int sleeping = CsdGetSleepOnIdle();
  /* 1: let the other PEs go idle, then measure process CPU over 1 s while
   * PE 0 spins: ~1 s if they sleep, ~n s if they spin */
  spinWait(0.3);
  double c0 = cpuSeconds(), w0 = CmiWallTimer();
  spinWait(1.0);
  double cpu = cpuSeconds() - c0, wall = CmiWallTimer() - w0;
  CmiPrintf("cpu %.2fs over %.2fs wall with %d PEs, sleep-on-idle %s\n", cpu, wall, n, sleeping ? "ON" : "off");
  if (sleeping && cpu > 1.0 + 0.25 * (n - 1))
    CmiAbort("idle PEs are still burning CPU with sleep-on-idle on\n");
  /* 2: wake sleeping PEs with messages; each must answer promptly */
  const int ROUNDS = 20;
  for (int r = 0; r < ROUNDS; r++) {
    spinWait(0.03); /* longer than the 10 ms max backoff: targets are asleep */
    for (int p = 1; p < n; p++) send(p, pingIdx, CmiWallTimer());
    double t0 = CmiWallTimer();
    while (pongs.load() < (r + 1) * (n - 1)) {
      CsdSchedulePoll();
      if (CmiWallTimer() - t0 > 2.0) CmiAbort("pongs never arrived\n");
    }
  }
  CmiPrintf("worst ping round trip to a %s PE: %.3f ms\n", sleeping ? "sleeping" : "spinning", worstRtt.load() * 1e3);
  if (sleeping && worstRtt.load() > 0.015) CmiAbort("wake-up latency above the 15 ms bound\n");
  for (int p = 0; p < n; p++) send(p, exitIdx, 0.0);
}
static void startfn(int, char **) {
  pingIdx = CmiRegisterHandler((CmiHandler)pingHandler);
  pongIdx = CmiRegisterHandler((CmiHandler)pongHandler);
  exitIdx = CmiRegisterHandler((CmiHandler)exitHandler);
  if (CmiMyRank() == 0) {
    CthThread d = CthCreate(driver, nullptr, 262144);
    CthAwaken(d);
  }
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ConverseInit(argc, argv, startfn);
  return 0;
}
