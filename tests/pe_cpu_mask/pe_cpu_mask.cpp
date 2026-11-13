// Regression test for reconverse#320. Without +pemap or +setcpuaffinity, every
// PE thread must start with the CPU mask the process had in main(). The
// network layer may bind the thread that opens it: psm2 (Omni-Path) binds a
// thread whose mask covers the whole node to a single CPU. PE threads are
// created later by that thread and inherit its mask, so before ConverseInit
// restored it, every PE of a process ran on one CPU, and tests whose PEs wait
// on each other took minutes instead of milliseconds.
//
// main() records the mask before ConverseInit. Each PE compares its own
// thread's mask with it and reports to PE 0, which prints the result and ends
// the run; a PE with a different mask fails the test with both masks printed.
// Where there is no sched_getaffinity (macOS, Windows) there is nothing to
// compare, and the test only checks that the run starts and exits.
#include "converse.h"
#include <cstdio>
#include <string>

#if defined(__linux__)
#include <sched.h>
#endif
#if defined(__linux__) && defined(CPU_SETSIZE)
#define PE_CPU_MASK_CHECK 1
static cpu_set_t processMask; // the main thread's mask in main()

static std::string maskString(const cpu_set_t &mask) {
  std::string s;
  for (int c = 0; c < CPU_SETSIZE; c++) {
    if (!CPU_ISSET(c, &mask))
      continue;
    int last = c;
    while (last + 1 < CPU_SETSIZE && CPU_ISSET(last + 1, &mask))
      last++;
    if (!s.empty())
      s += ",";
    s += std::to_string(c);
    if (last > c)
      s += "-" + std::to_string(last);
    c = last;
  }
  return s.empty() ? "(empty)" : s;
}
#else
#define PE_CPU_MASK_CHECK 0
#endif

struct Msg {
  CmiMessageHeader header;
  int pe;
  int same; // 1 if this PE's mask equals the process's mask in main()
};

CpvDeclare(int, reportIdx);
CpvDeclare(int, exitIdx);
CpvDeclare(int, reports);
CpvDeclare(int, mismatches);

static void report_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  if (!m->same)
    CpvAccess(mismatches)++;
  CmiFree(vmsg);
  if (++CpvAccess(reports) < CmiNumPes())
    return;
  if (CpvAccess(mismatches) != 0)
    CmiAbort("pe_cpu_mask: %d of %d PEs started with a CPU mask other than "
             "the process's mask in main() (reconverse#320)",
             CpvAccess(mismatches), CmiNumPes());
#if PE_CPU_MASK_CHECK
  printf("pe_cpu_mask: all %d PEs started with the process's CPU mask (%s)\n",
         CmiNumPes(), maskString(processMask).c_str());
#else
  printf("pe_cpu_mask: no per-thread CPU masks on this platform; %d PEs "
         "started\n",
         CmiNumPes());
#endif
  Msg *e = (Msg *)CmiAlloc(sizeof(Msg));
  e->header.messageSize = sizeof(Msg);
  CmiSetHandler(e, CpvAccess(exitIdx));
  CmiSyncBroadcastAllAndFree(sizeof(Msg), e);
}

static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, reportIdx);
  CpvInitialize(int, exitIdx);
  CpvInitialize(int, reports);
  CpvInitialize(int, mismatches);
  CpvAccess(reports) = 0;
  CpvAccess(mismatches) = 0;
  CpvAccess(reportIdx) = CmiRegisterHandler(report_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);

  int same = 1;
#if PE_CPU_MASK_CHECK
  cpu_set_t mine;
  if (sched_getaffinity(0, sizeof(mine), &mine) != 0)
    CmiAbort("pe_cpu_mask: sched_getaffinity failed on PE %d", CmiMyPe());
  same = CPU_EQUAL(&mine, &processMask);
  if (!same)
    fprintf(stderr,
            "pe_cpu_mask: PE %d (process %d, rank %d) started with CPUs %s; "
            "the process had %s in main()\n",
            CmiMyPe(), CmiMyNode(), CmiMyRank(), maskString(mine).c_str(),
            maskString(processMask).c_str());
#endif

  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  m->pe = CmiMyPe();
  m->same = same;
  CmiSetHandler(m, CpvAccess(reportIdx));
  CmiSyncSendAndFree(0, sizeof(Msg), m);
}

int main(int argc, char **argv) {
#if PE_CPU_MASK_CHECK
  if (sched_getaffinity(0, sizeof(processMask), &processMask) != 0) {
    perror("pe_cpu_mask: sched_getaffinity");
    return 1;
  }
#endif
  ConverseInit(argc, argv, mymain);
  return 0;
}
