// CmiInitCPUAffinity must consume its own command-line flags whether or not
// this build has hwloc.
//
// Reconverse's ConverseInit does not call CmiInitCPUAffinity; callers do, the
// way Charm++'s init.C does. Right after that call, init.C scans what is left
// of argv and warns about anything still starting with '+':
//
//   WARNING: +showcpuaffinity is a command line argument beginning with a '+'
//   but was not parsed by the RTS.
//
// When RECONVERSE_ENABLE_CPU_AFFINITY was off (hwloc missing at configure
// time), CmiInitCPUAffinity was an empty stub, so +setcpuaffinity, +pemap and
// +showcpuaffinity all survived in argv and drew that warning -- which points
// at the argument rather than at the missing hwloc. This test pins the
// contract that holds in both builds: the flags are always removed from argv.
// Whether the binding actually happens is a separate question, checked by
// runtime_modes where affinity is compiled in.
#include "converse.h"
#include "converse_config.h" // RECONVERSE_ENABLE_CPU_AFFINITY; converse.h does not expose it
#include <cstdio>
#include <cstring>

static void mymain(int argc, char **argv) {
  // ctest passes: +setcpuaffinity +pemap L0 +showcpuaffinity
  int before = CmiGetArgc(argv);
  if (before < 4)
    CmiAbort("cpuaffinity_args needs its arguments; run it through ctest");

  CmiInitCPUAffinity(argv);

  // Every affinity flag must be gone, on every PE, in either build.
  int leftover = 0;
  for (int i = 1; argv[i] != NULL; i++) {
    if (argv[i][0] == '+') {
      CmiPrintf("cpuaffinity_args: PE %d: '%s' survived CmiInitCPUAffinity\n",
                CmiMyPe(), argv[i]);
      leftover++;
    }
  }
  if (leftover != 0)
    CmiAbort("cpuaffinity_args: %d affinity argument(s) left in argv on PE %d; "
             "a caller such as Charm++'s init.C reports these as unparsed "
             "'+' arguments",
             leftover, CmiMyPe());

  // The value argument of +pemap must be consumed along with the flag, not
  // left behind as a stray bare word.
  for (int i = 1; argv[i] != NULL; i++)
    if (strcmp(argv[i], "L0") == 0)
      CmiAbort("cpuaffinity_args: +pemap's value 'L0' was left in argv on "
               "PE %d",
               CmiMyPe());

  if (CmiMyPe() == 0) {
    CmiPrintf("cpuaffinity_args: all affinity flags consumed (%d -> %d args), "
              "affinity %s in this build\n",
              before, CmiGetArgc(argv),
#ifdef RECONVERSE_ENABLE_CPU_AFFINITY
              "ENABLED"
#else
              "DISABLED (no hwloc)"
#endif
    );
    CmiPrintf("All tests passed\n");
  }
  CmiExit(0);
}

int main(int argc, char **argv) { ConverseInit(argc, argv, mymain); }
