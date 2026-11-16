// A broadcast must reach every PE of a process while that process's rank 0
// is blocked in CmiNodeBarrier before its scheduler runs (#261).
//
// PE 0 broadcasts. In every other process, rank 0 enters CmiNodeBarrier
// straight from the start function, and the other ranks wait for the
// broadcast before they enter the barrier: the pattern of Charm++'s restart
// (charm#4018), where _initDone's node barrier on rank 0 met peers waiting
// for a broadcast. A rank 0 that has to run a handler before its peers get
// their copies deadlocks here. A peer that waits longer than the timeout
// aborts the run instead of hanging it.
//
// With 6 or more processes, the copy for process 1 (a child of the root)
// carries the copies for processes 5 and up, so a blocked rank 0 that
// forwards in a handler also starves its child processes.
//
// With +ipc +expect-ipc, PE 0 must have sent the copy for process 1 through
// the shared-memory pool, so that path is the one tested.
//
// Usage: <launcher> -n P ./bcast_rank0_barrier +pe P*R [+ipc +expect-ipc]
//        [timeout s]   (R >= 2 PEs per process)
#include <converse.h>
#include <stdlib.h>

struct BcastMsg {
  char core[CmiMsgHeaderSizeBytes];
  int magic;
};

struct DoneMsg {
  char core[CmiMsgHeaderSizeBytes];
  int pe;
};

static const int kMagic = 0x261;
static int bcastIdx, doneIdx;
static double timeout = 20;
static int expectIpc;

CpvStaticDeclare(int, got);
CpvStaticDeclare(int, done);

static void bcastHandler(void *msg) {
  if (((BcastMsg *)msg)->magic != kMagic)
    CmiAbort("bcast_rank0_barrier: PE %d: corrupt broadcast\n", CmiMyPe());
  CmiFree(msg);
  CpvAccess(got)++;
  DoneMsg *d = (DoneMsg *)CmiAlloc(sizeof(DoneMsg));
  CmiSetHandler(d, doneIdx);
  d->pe = CmiMyPe();
  CmiSyncSendAndFree(0, sizeof(DoneMsg), d);
}

static void doneHandler(void *msg) {
  CmiFree(msg);
  if (++CpvAccess(done) < CmiNumPes())
    return;
  if (expectIpc && CmiIpcMessagesSent() == 0)
    CmiAbort("bcast_rank0_barrier: +expect-ipc, but PE 0 sent nothing through "
             "the shared-memory pool\n");
  CmiPrintf("bcast_rank0_barrier: %d processes x %d PEs, ipc %s: every PE "
            "received the broadcast PASS\n", CmiNumNodes(), CmiMyNodeSize(),
            CmiIpcEnabled() ? "on" : "off");
  CmiExit(0);
}

static void mymain(int argc, char **argv) {
  bcastIdx = CmiRegisterHandler(bcastHandler);
  doneIdx = CmiRegisterHandler(doneHandler);
  expectIpc = CmiGetArgFlagDesc(argv, "+expect-ipc",
                                "fail unless the pool carried the broadcast");
  argc = CmiGetArgc(argv);
  if (argc > 1)
    timeout = atof(argv[1]);
  CpvInitialize(int, got);
  CpvInitialize(int, done);
  CpvAccess(got) = CpvAccess(done) = 0;
  if (CmiNumNodes() < 2 || CmiMyNodeSize() < 2)
    CmiAbort("bcast_rank0_barrier: needs at least 2 processes of at least 2 "
             "PEs each\n");
  if (expectIpc && !CmiIpcEnabled())
    CmiAbort("bcast_rank0_barrier: +expect-ipc, but the shared-memory pool is "
             "not up\n");

  // every PE of every process registers its handlers before anyone sends
  CmiBarrier();

  if (CmiMyPe() == 0) {
    BcastMsg *m = (BcastMsg *)CmiAlloc(sizeof(BcastMsg));
    CmiSetHandler(m, bcastIdx);
    m->magic = kMagic;
    CmiSyncBroadcastAllAndFree(sizeof(BcastMsg), m);
  }
  if (CmiMyNode() == 0)
    return;

  if (CmiMyRank() == 0) {
    // Not scheduling: its own copy stays queued until the barrier is over.
    CmiNodeBarrier();
    return;
  }
  const double deadline = CmiWallTimer() + timeout;
  while (CpvAccess(got) == 0) {
    CsdSchedulePoll();
    if (CmiWallTimer() > deadline)
      CmiAbort("bcast_rank0_barrier: PE %d (process %d) received no broadcast "
               "in %.0f s while its rank 0 waited in CmiNodeBarrier\n",
               CmiMyPe(), CmiMyNode(), timeout);
  }
  CmiNodeBarrier();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
