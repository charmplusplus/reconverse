// The CmiMemoryUsage family: a probe answers, usage follows a large block as
// it is touched and freed, the high- and low-water marks see the samples taken
// between their resets, and the peak resident size covers the block. PE 0
// runs the checks; the figures are per process, so one PE is enough.
#include "converse.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("memory_usage test: %s", what);
}

static double mib(CmiUInt8 bytes) { return bytes / (1024.0 * 1024.0); }

// The block is written and freed without being read, so a compiler may drop
// the malloc, the memset and the free together; going through a volatile
// pointer makes the block escape.
static char *volatile blockPtr;

static void mymain(int argc, char **argv) {
  if (CmiMyPe() == 0) {
    const CmiUInt8 block = 64 << 20;
    const CmiUInt8 slack = 16 << 20; // other threads, page rounding

    const char *reporter = CmiMemoryUsageReporter();
    check(reporter && strcmp(reporter, "none") != 0, "no probe answered");
    const CmiUInt8 before = CmiMemoryUsage();
    check(before > 0, "CmiMemoryUsage returned 0");

    CmiResetMaxMemory();
    blockPtr = (char *)malloc(block);
    char *p = blockPtr;
    check(p != NULL, "malloc of the block failed");
    memset(p, 1, block); // resident, not just reserved
    const CmiUInt8 during = CmiMemoryUsage();
    check(during >= before + block - slack, "usage did not grow by the block");

    CmiResetMinMemory(); // at the high point
    free(p);
    const CmiUInt8 after = CmiMemoryUsage();
    check(after + block - slack <= during, "usage did not fall after free");

    const CmiUInt8 maxSeen = CmiMaxMemoryUsage();
    check(maxSeen >= during, "high-water mark below a sample since its reset");
    const CmiUInt8 minSeen = CmiMinMemoryUsage();
    check(minSeen <= after, "low-water mark above a sample since its reset");

    const CmiUInt8 peakResident = CmiMaxMemoryUsageR();
    check(peakResident >= block, "peak resident size below the block");

    CmiPrintf("memory_usage: %s: before %.1f, with the block %.1f, after free "
              "%.1f, max %.1f, min %.1f, peak resident %.1f MiB\n",
              reporter, mib(before), mib(during), mib(after), mib(maxSeen),
              mib(minSeen), mib(peakResident));
    CmiPrintf("memory_usage: PASS\n");
  }
  CsdExitScheduler();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
