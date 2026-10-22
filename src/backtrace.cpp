/* Stack traces for CmiAbort and for fatal signals.
 *
 * CmiPrintStackTrace prints the calling thread's stack to stderr, one frame
 * per line with demangled names resolved through dladdr. It runs in ordinary
 * context (the CmiAbort path), so it may allocate.
 *
 * The fatal-signal handler (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) uses
 * only backtrace_symbols_fd, which allocates nothing, then re-raises the
 * signal with its default disposition restored, so the exit status and any
 * core dump are those of the original fault. +truecrash leaves the handlers
 * uninstalled, for a debugger or a core dump that should see the fault
 * first-hand.
 *
 * Symbols inside the executable itself resolve on Linux only when it is
 * linked with -rdynamic (reconverse's CMake interface adds it to consumers);
 * otherwise a frame shows the object and an address for addr2line.
 * 
 * need to do in future: handle possibility of stack overflow in user level threads
 * when implementing AMPI on Reconverse
 */
#include "converse_internal.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

static const int kMaxFrames = 64;

void CmiPrintStackTrace(int nSkip) {
  void *frames[kMaxFrames];
  const int n = backtrace(frames, kMaxFrames);
  nSkip += 1; // this function's own frame
  fprintf(stderr, "[%d] Stack trace (most recent call first):\n", CmiMyPe());
  for (int i = nSkip; i < n; i++) {
    Dl_info info;
    const char *name = nullptr;
    const char *object = "?";
    char *demangled = nullptr;
    if (dladdr(frames[i], &info)) {
      if (info.dli_sname) {
        int status = 0;
        demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        name = (status == 0 && demangled) ? demangled : info.dli_sname;
      }
      if (info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        object = slash ? slash + 1 : info.dli_fname;
      }
    }
    fprintf(stderr, "[%d]   #%-2d %s (%s) [%p]\n", CmiMyPe(), i - nSkip,
            name ? name : "??", object, frames[i]);
    free(demangled);
  }
  fflush(stderr);
}

static const char *signalName(int sig) {
  switch (sig) {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS: return "SIGBUS";
  case SIGILL: return "SIGILL";
  case SIGFPE: return "SIGFPE";
  case SIGABRT: return "SIGABRT";
  default: return "signal";
  }
}

static void fatalSignalHandler(int sig) {
  // Async-signal-safe only: no malloc, no stdio.
  char line[96];
  int len = snprintf(line, sizeof line,
                     "[%d] Fatal signal %d (%s); stack trace (most recent call first):\n",
                     CmiMyPe(), sig, signalName(sig));
  if (len > 0) {
    ssize_t w = write(STDERR_FILENO, line, (size_t)len);
    (void)w;
  }
  void *frames[kMaxFrames];
  const int n = backtrace(frames, kMaxFrames);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  // SA_RESETHAND has restored the default disposition; re-raise so the
  // process ends the way the fault would have ended it.
  raise(sig);
}

void CmiInitCrashHandlers(char **argv) {
  if (CmiGetArgFlagDesc(argv, "+truecrash",
                        "Leave fatal signals uncaught: no stack trace, the "
                        "original fault reaches the debugger or core dump"))
    return;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fatalSignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  for (int s : signals)
    sigaction(s, &sa, nullptr);
}
