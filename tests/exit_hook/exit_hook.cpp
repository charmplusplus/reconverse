// CmiSetExitHook. Every PE calls ConverseExit(5). The hook must run once per
// process, with the exit code, on the thread that called ConverseInit, and
// the process must then exit with that code.
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <thread>

static std::thread::id mainThread;
static std::atomic<int> hookCalls{0};

static void hook(int exitcode) {
  int call = ++hookCalls;
  printf("exit_hook: hook call %d, code %d, %s\n", call, exitcode,
         std::this_thread::get_id() == mainThread ? "main thread" : "NOT the main thread");
  fflush(stdout);
}

CmiStartFn mymain(int argc, char **argv) {
  CmiNodeBarrier();
  ConverseExit(5);
  return 0;
}

int main(int argc, char **argv) {
  mainThread = std::this_thread::get_id();
  CmiSetExitHook(hook); // before ConverseInit, as an MPI program would
  ConverseInit(argc, argv, (CmiStartFn)mymain);
}
