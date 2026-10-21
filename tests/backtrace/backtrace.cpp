// Checks that a crash leaves a stack trace on stderr. With no argument a
// function three frames deep calls CmiAbort; with "segv" it dereferences a
// null pointer so the fatal-signal handler runs; with "+truecrash segv" the
// handler is not installed and the process dies without a trace. The ctest
// entries look for the function name in the output.
#include "converse.h"
#include <stdio.h>
#include <string.h>

static volatile int *null_pointer = NULL;

__attribute__((noinline)) void backtrace_test_leaf(int how) {
  if (how == 1)
    *null_pointer = 42;
  CmiAbort("backtrace test: deliberate abort");
}

__attribute__((noinline)) void backtrace_test_middle(int how) {
  backtrace_test_leaf(how);
  printf("unreachable %d\n", how);
}

CmiStartFn mymain(int argc, char **argv) {
  int how = (argc > 1 && strcmp(argv[1], "segv") == 0) ? 1 : 0;
  if (CmiMyPe() == 0)
    backtrace_test_middle(how);
  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
