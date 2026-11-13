// CmiSetAbortHook. Every PE calls CmiAbort at once. The hook must run once
// per process, with code 1 and CmiAbort's formatted reason, and the other PEs
// must wait for it rather than run it again or abort the process under it.
// The hook first waits until every PE of its process has called CmiAbort, so
// that they are all there while it runs.
//
//   abort_hook +pe 4         the hook returns; CmiAbort then aborts as usual
//   abort_hook +pe 4 exit    the hook ends the process with status 3, as
//                            MPI_Abort ends an MPI job
//   abort_hook +pe 4 block   the hook never returns; the other PEs stop
//                            waiting for it after 10 s and abort the process
#include "converse.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

enum Mode { RETURN, EXIT, BLOCK };
static Mode mode = RETURN;          // set in main, before the PE threads exist
static std::atomic<int> hookCalls{0};
static std::atomic<int> aborting{0}; // PEs of this process that called CmiAbort

static void hook(int code, const char *msg) {
  int call = ++hookCalls;
  fprintf(stderr, "abort_hook: hook entered, call %d\n", call);
  fflush(stderr);
  auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (aborting.load() < CmiMyNodeSize() && std::chrono::steady_clock::now() < limit)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  // Time for the other PEs to get from CmiAbort's printing to the hook.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  if (mode == BLOCK) {
    fprintf(stderr, "abort_hook: hook blocks: %d of %d PEs aborting\n", aborting.load(),
            CmiMyNodeSize());
    fflush(stderr);
    for (;;)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  const char *what = mode == EXIT ? "ends the process" : "returns";
  fprintf(stderr, "abort_hook: hook %s: calls %d, code %d, reason [%s], %d of %d PEs aborting\n",
          what, hookCalls.load(), code, msg, aborting.load(), CmiMyNodeSize());
  fflush(stderr);
  if (mode == EXIT)
    std::_Exit(3);
}

CmiStartFn mymain(int argc, char **argv) {
  CmiNodeBarrier(); // so that every PE calls CmiAbort at the same time
  ++aborting;
  CmiAbort("abort_hook: PE %d aborts", CmiMyPe());
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "exit") == 0)
      mode = EXIT;
    if (strcmp(argv[i], "block") == 0)
      mode = BLOCK;
  }
  CmiSetAbortHook(hook); // before ConverseInit, as an MPI program would
  ConverseInit(argc, argv, (CmiStartFn)mymain);
}
