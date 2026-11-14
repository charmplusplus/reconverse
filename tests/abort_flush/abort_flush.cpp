// CmiRegisterAbortFlush. Every PE calls CmiAbort at once. Each registered
// function must run once per process, in the order registered and before the
// abort hook, and the other PEs must wait for them rather than abort the
// process during them. Function A is registered twice and must still run once.
// A waits until every PE of its process has called CmiAbort, so that they are
// all there while the step runs; B then sleeps, so that an aborting PE that
// did not wait would end the process before B prints its line.
//
//   abort_flush +pe 4          the functions run, then the hook
//   abort_flush +pe 4 nested   A calls CmiAbort: that call skips the step
//                              (B does not run) and goes on to the hook
#include "converse.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static bool nested = false;          // set in main, before the PE threads exist
static std::atomic<int> callsA{0};
static std::atomic<int> callsB{0};
static std::atomic<int> order{0};    // calls so far, A and B together
static std::atomic<int> aborting{0}; // PEs of this process that called CmiAbort

static int flushA(void) {
  int call = ++callsA;
  ++order;
  fprintf(stderr, "abort_flush: A entered, call %d\n", call);
  fflush(stderr);
  auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (aborting.load() < CmiMyNodeSize() && std::chrono::steady_clock::now() < limit)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (nested)
    CmiAbort("abort_flush: CmiAbort inside A");
  return 0;
}

static int flushB(void) {
  int call = ++callsB;
  int pos = ++order;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  fprintf(stderr, "abort_flush: B done, call %d, position %d, A calls %d\n", call, pos,
          callsA.load());
  fflush(stderr);
  return 0;
}

static void hook(int code, const char *msg) {
  fprintf(stderr, "abort_flush: hook: A calls %d, B calls %d, %d of %d PEs aborting\n",
          callsA.load(), callsB.load(), aborting.load(), CmiMyNodeSize());
  fflush(stderr);
}

CmiStartFn mymain(int argc, char **argv) {
  CmiNodeBarrier(); // so that every PE calls CmiAbort at the same time
  ++aborting;
  CmiAbort("abort_flush: PE %d aborts", CmiMyPe());
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "nested") == 0)
      nested = true;
  CmiRegisterAbortFlush(flushA); // before ConverseInit
  CmiRegisterAbortFlush(flushB);
  CmiRegisterAbortFlush(flushA);
  CmiSetAbortHook(hook);
  ConverseInit(argc, argv, (CmiStartFn)mymain);
}
