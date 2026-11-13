// CmiSetAbortHook (reconverse #321). Every PE calls CmiAbort at once. The
// hook must run once per process, with code 1 and CmiAbort's formatted
// reason, and the other PEs must wait for it rather than run it again or
// abort the process under it.
//
//   abort_hook +pe 4        the hook waits 300 ms and returns; CmiAbort then
//                           aborts as it does without a hook
//   abort_hook +pe 4 exit   the hook ends the process with status 3 after the
//                           wait, as MPI_Abort ends an MPI job
#include "converse.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static std::atomic<int> hookCalls{0};
static bool exitInHook = false; // set in main, before the PE threads exist

static void hook(int code, const char *msg) {
  int call = ++hookCalls;
  fprintf(stderr, "abort_hook: hook entered, call %d\n", call);
  fflush(stderr);
  // Long enough for every other PE to reach CmiAbort: without the wait there
  // they would enter the hook too, or abort the process from under it.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (exitInHook) {
    fprintf(stderr, "abort_hook: hook ends the process: calls %d, code %d, reason [%s]\n",
            hookCalls.load(), code, msg);
    fflush(stderr);
    std::_Exit(3);
  }
  fprintf(stderr, "abort_hook: hook returns: calls %d, code %d, reason [%s]\n",
          hookCalls.load(), code, msg);
  fflush(stderr);
}

CmiStartFn mymain(int argc, char **argv) {
  CmiNodeBarrier(); // so that every PE calls CmiAbort at the same time
  CmiAbort("abort_hook: PE %d aborts", CmiMyPe());
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "exit") == 0)
      exitInHook = true;
  CmiSetAbortHook(hook); // before ConverseInit, as an MPI program would
  ConverseInit(argc, argv, (CmiStartFn)mymain);
}
