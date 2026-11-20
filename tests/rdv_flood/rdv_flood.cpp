// Many large sends posted from one handler, to and from every process at once
// (issue #257).
//
// Every PE sends `count` messages of `bytes` to the PE with the same rank in
// the next process, all from its start function, i.e. without returning to
// the scheduler in between, the way an application entry method sends its
// share of an all-to-all. With the LCI backend a send larger than LCI's
// buffer-copy limit (8116 bytes at the default 8 KiB packet) goes by
// rendezvous, and each one in flight holds a packet of the pool that receives
// are posted from, until the destination's ready-to-receive arrives. Without a
// bound on those sends a process can hand its whole pool to them; it can then
// post no receive, so the ready-to-receive messages that would release them
// are never seen, and the run hangs with LCI printing "Deadlock alert! The
// device does not have any posted recvs" in a loop. A small pool and few
// preposted receives (LCI_ATTR_NPACKETS=2048, LCI_ATTR_NET_MAX_RECVS=256) make
// that happen at two processes of two PEs.
//
// Each PE reports to PE 0 once it has received its `count` messages; PE 0
// ends the run when every PE has. The test passes if the run completes.
//
// Usage: rdv_flood [messages per PE = 4000] [bytes = 16384]
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

static int data_handler, done_handler, exit_handler;
static long count = 4000;
static int bytes = 16384;
CpvStaticDeclare(long, received);
static int pes_done = 0;  // PE 0 only
static double start_time;

struct Hdr {
  char core[CmiMsgHeaderSizeBytes];
};

static void exitHandler(void *msg) {
  CmiFree(msg);
  CsdExitScheduler();
}

static void doneHandler(void *msg) {
  CmiFree(msg);
  if (++pes_done < CmiNumPes()) return;
  double t = CmiWallTimer() - start_time;
  CmiPrintf("rdv_flood: %d PEs in %d processes, %ld x %d-byte messages per PE, "
            "%.3f s, %.2f GB/s per process: PASS\n",
            CmiNumPes(), CmiNumNodes(), count, bytes, t,
            (double)count * bytes * CmiMyNodeSize() / t / 1e9);
  Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
  CmiSetHandler(m, exit_handler);
  CmiSyncBroadcastAllAndFree(sizeof(Hdr), m);
}

static void dataHandler(void *msg) {
  CmiFree(msg);
  if (++CpvAccess(received) < count) return;
  Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
  CmiSetHandler(m, done_handler);
  CmiSyncSendAndFree(0, sizeof(Hdr), m);
}

static void start(int argc, char **argv) {
  CpvInitialize(long, received);
  CpvAccess(received) = 0;
  data_handler = CmiRegisterHandler(dataHandler);
  done_handler = CmiRegisterHandler(doneHandler);
  exit_handler = CmiRegisterHandler(exitHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1) count = atol(argv[1]);
  if (argc > 2) bytes = atoi(argv[2]);
  if (bytes < (int)sizeof(Hdr)) bytes = sizeof(Hdr);

  if (CmiNumNodes() < 2) {
    if (CmiMyPe() == 0)
      CmiPrintf("rdv_flood: needs at least 2 processes; nothing to do\n");
    CsdExitScheduler();
    return;
  }
  // every PE of every process has its handlers before anyone sends
  CmiBarrier();

  if (CmiMyPe() == 0) {
    start_time = CmiWallTimer();
    CmiPrintf("rdv_flood: %d PEs in %d processes, %ld x %d-byte messages per "
              "PE to the next process\n",
              CmiNumPes(), CmiNumNodes(), count, bytes);
  }
  int dest = (CmiMyPe() + CmiMyNodeSize()) % CmiNumPes();
  for (long i = 0; i < count; i++) {
    char *m = (char *)CmiAlloc(bytes);
    CmiSetHandler(m, data_handler);
    CmiSyncSendAndFree(dest, bytes, m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, start, 0, 0);
  return 0;
}
