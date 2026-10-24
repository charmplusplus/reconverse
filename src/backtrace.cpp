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

static const int kMaxFrames = 64;

#ifdef _WIN32
/* Windows: frames come from CaptureStackBackTrace and are named through
 * DbgHelp, which finds function names (and, when the program was linked with
 * a PDB, files and lines); a frame it cannot name shows module+offset for
 * lookup in a debugger. Fatal faults arrive as structured exceptions rather
 * than signals, so an unhandled-exception filter stands in for the signal
 * handler, and the Windows Error Reporting and abort() dialogs are turned off
 * so that a crashed run exits instead of waiting for someone to click. */
#include "win32_compat.h"
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// DbgHelp is single-threaded.
static SRWLOCK symLock = SRWLOCK_INIT;
static bool symReady = false;

static void printFrames(void *const *frames, int n) {
  HANDLE proc = GetCurrentProcess();
  AcquireSRWLockExclusive(&symLock);
  if (!symReady) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    symReady = SymInitialize(proc, NULL, TRUE) != FALSE;
  }
  alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
  for (int i = 0; i < n; i++) {
    const DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
    char module[MAX_PATH] = "?";
    HMODULE hmod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)frames[i], &hmod) &&
        GetModuleFileNameA(hmod, module, sizeof module)) {
      const char *slash = strrchr(module, '\\');
      if (slash) memmove(module, slash + 1, strlen(slash + 1) + 1);
    }
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
    memset(sym, 0, sizeof(SYMBOL_INFO));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 symOff = 0;
    IMAGEHLP_LINE64 line;
    memset(&line, 0, sizeof line);
    line.SizeOfStruct = sizeof line;
    DWORD lineOff = 0;
    if (symReady && SymFromAddr(proc, addr, &symOff, sym)) {
      if (SymGetLineFromAddr64(proc, addr, &lineOff, &line))
        fprintf(stderr, "[%d]   #%-2d %s (%s) [%s:%lu]\n", CmiMyPe(), i,
                sym->Name, module, line.FileName, (unsigned long)line.LineNumber);
      else
        fprintf(stderr, "[%d]   #%-2d %s+0x%llx (%s) [%p]\n", CmiMyPe(), i,
                sym->Name, (unsigned long long)symOff, module, frames[i]);
    } else {
      fprintf(stderr, "[%d]   #%-2d ?? (%s+0x%llx) [%p]\n", CmiMyPe(), i, module,
              (unsigned long long)(addr - (DWORD64)(uintptr_t)hmod), frames[i]);
    }
  }
  ReleaseSRWLockExclusive(&symLock);
  fflush(stderr);
}

void CmiPrintStackTrace(int nSkip) {
  void *frames[kMaxFrames];
  // + 1: this function's own frame
  const int n = CaptureStackBackTrace((DWORD)(nSkip + 1), kMaxFrames, frames, NULL);
  fprintf(stderr, "[%d] Stack trace (most recent call first):\n", CmiMyPe());
  printFrames(frames, n);
}

// The exception, and the POSIX signal the same fault raises elsewhere, so
// that a report reads (and greps) the same on every platform.
static const char *exceptionName(DWORD code, const char **signame) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION: *signame = "SIGSEGV"; return "access violation";
  case EXCEPTION_STACK_OVERFLOW: *signame = "SIGSEGV"; return "stack overflow";
  case EXCEPTION_IN_PAGE_ERROR: *signame = "SIGBUS"; return "in-page error";
  case EXCEPTION_DATATYPE_MISALIGNMENT: *signame = "SIGBUS"; return "misaligned data";
  case EXCEPTION_ILLEGAL_INSTRUCTION: *signame = "SIGILL"; return "illegal instruction";
  case EXCEPTION_PRIV_INSTRUCTION: *signame = "SIGILL"; return "privileged instruction";
  case EXCEPTION_INT_DIVIDE_BY_ZERO: *signame = "SIGFPE"; return "integer divide by zero";
  default: *signame = "signal"; return "exception";
  }
}

static LONG WINAPI fatalExceptionFilter(EXCEPTION_POINTERS *ep) {
  const EXCEPTION_RECORD *rec = ep->ExceptionRecord;
  const char *signame;
  const char *what = exceptionName(rec->ExceptionCode, &signame);
  fprintf(stderr, "[%d] Fatal signal (%s): %s, exception 0x%08lx at %p",
          CmiMyPe(), signame, what, (unsigned long)rec->ExceptionCode,
          rec->ExceptionAddress);
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      rec->NumberParameters >= 2)
    fprintf(stderr, " (%s address %p)",
            rec->ExceptionInformation[0] == 0   ? "reading"
            : rec->ExceptionInformation[0] == 8 ? "executing"
                                                : "writing",
            (void *)rec->ExceptionInformation[1]);
  fprintf(stderr, "\n");
  // A stack overflow leaves too little stack to walk safely.
  if (rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
    void *frames[kMaxFrames];
    // The filter runs on the faulting thread, so this is its stack, beneath
    // the exception dispatcher's own frames. Start at the faulting
    // instruction when it can be found.
    const int n = CaptureStackBackTrace(0, kMaxFrames, frames, NULL);
    int first = 0;
    for (int i = 0; i < n; i++)
      if (frames[i] == rec->ExceptionAddress) {
        first = i;
        break;
      }
    fprintf(stderr, "[%d] Stack trace (most recent call first):\n", CmiMyPe());
    printFrames(frames + first, n - first);
  }
  fflush(stderr);
  // End the process with the exception code, as the default handling would,
  // but without Windows Error Reporting.
  TerminateProcess(GetCurrentProcess(), (UINT)rec->ExceptionCode);
  return EXCEPTION_CONTINUE_SEARCH;
}

static void abortSignalHandler(int sig) {
  fprintf(stderr, "[%d] Fatal signal %d (SIGABRT); stack trace (most recent "
                  "call first):\n", CmiMyPe(), sig);
  CmiPrintStackTrace(1);
  signal(SIGABRT, SIG_DFL);
  raise(sig);
}

void CmiInitCrashHandlers(char **argv) {
  if (CmiGetArgFlagDesc(argv, "+truecrash",
                        "Leave fatal signals uncaught: no stack trace, the "
                        "original fault reaches the debugger or core dump"))
    return;
  // No "abort() has been called" or fault-reporting dialog box: a crashed
  // run on a desktop must still exit by itself.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  SetUnhandledExceptionFilter(fatalExceptionFilter);
  signal(SIGABRT, abortSignalHandler);
}

#else /* !_WIN32 */

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

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

#endif /* _WIN32 */
