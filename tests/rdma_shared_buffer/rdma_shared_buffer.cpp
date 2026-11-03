// Every PE of one process puts from the same registered buffer at once.
// The LCI backend registers a buffer on a device the first time a transfer
// needs that device, so the putters race to register the shared buffer on
// each target's device; one registration must win and the others be
// released. Two processes of R PEs. Each round, every PE of process 0
// registers a fresh destination and sends it to the coordinator (rank 0 of
// process 1), which fills and registers the shared source and hands both to
// all of its PEs. Each of them puts the source into every destination. The
// bytes of every destination are checked, then everything is deregistered.
//
// Usage: <launcher> -n 2 ./reconverse_rdma_shared_buffer +pe 2R
//        [+lci_ndevices D] [rounds, default 100]
#include "conv-rdma.h"
#include <converse.h>
#include <stdlib.h>
#include <string.h>

static const size_t kBytes = 65536;
static const int kMaxPes = 64;

struct TargetMsg {
  char core[CmiMsgHeaderSizeBytes];
  int round;
  int target; // rank in process 0
  CmiNcpyBuffer dst;
};

struct PutMsg {
  char core[CmiMsgHeaderSizeBytes];
  int round;
  int ntargets;
  CmiNcpyBuffer src;
  CmiNcpyBuffer dst[kMaxPes];
};

struct RoundMsg {
  char core[CmiMsgHeaderSizeBytes];
  int round;
};

static int targetIdx, putIdx, ackIdx, doneIdx, finishIdx;
static int rounds = 100;

// process 1, owned by the coordinator
CsvStaticDeclare(char *, src);
CsvStaticDeclare(CmiNcpyBuffer, srcBuf);
CsvStaticDeclare(PutMsg *, pending);
CsvStaticDeclare(int, targetsIn);
CsvStaticDeclare(int, acks);

// process 0, one per PE
CpvStaticDeclare(char *, dst);
CpvStaticDeclare(CmiNcpyBuffer, dstBuf);
CpvStaticDeclare(int, finished);

static char pattern(int round, size_t i) {
  return (char)((round * 13 + i) & 0xff);
}

static int coordinator() { return CmiNodeFirst(1); }

static void startRound(int round) {
  memset(CpvAccess(dst), 0, kBytes);
  CpvAccess(dstBuf) = CmiNcpyBuffer(CpvAccess(dst), kBytes);
  TargetMsg *m = (TargetMsg *)CmiAlloc(sizeof(TargetMsg));
  CmiSetHandler(m, targetIdx);
  m->round = round;
  m->target = CmiMyRank();
  m->dst = CpvAccess(dstBuf);
  CmiSyncSendAndFree(coordinator(), sizeof(TargetMsg), m);
}

// coordinator: a destination for this round
static void targetHandler(void *msg) {
  TargetMsg *m = (TargetMsg *)msg;
  PutMsg *p = CsvAccess(pending);
  p->round = m->round;
  p->dst[m->target] = m->dst;
  CmiFree(msg);
  if (++CsvAccess(targetsIn) < CmiNodeSize(0))
    return;
  CsvAccess(targetsIn) = 0;
  CsvAccess(acks) = 0;
  for (size_t i = 0; i < kBytes; i++)
    CsvAccess(src)[i] = pattern(p->round, i);
  // a fresh registration each round, so its devices fill in all over again
  CsvAccess(srcBuf) = CmiNcpyBuffer(CsvAccess(src), kBytes);
  p->src = CsvAccess(srcBuf);
  p->ntargets = CmiNodeSize(0);
  for (int j = 0; j < CmiMyNodeSize(); j++) {
    PutMsg *copy = (PutMsg *)CmiAlloc(sizeof(PutMsg));
    memcpy(copy, p, sizeof(PutMsg));
    CmiSetHandler(copy, putIdx);
    CmiSyncSendAndFree(coordinator() + j, sizeof(PutMsg), copy);
  }
}

// every PE of process 1
static void putHandler(void *msg) {
  PutMsg *p = (PutMsg *)msg;
  int n = p->ntargets;
  for (int k = 0; k < n; k++) {
    int i = (k + CmiMyRank()) % n;
    p->src.rdmaPut(p->dst[i], sizeof(int), (char *)&p->round,
                   (char *)&p->round);
  }
  CmiFree(msg);
}

// Runs on the thread that polled the completion, possibly not the initiator.
static void ncpyAckHandler(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;
  RoundMsg *m = (RoundMsg *)CmiAlloc(sizeof(RoundMsg));
  CmiSetHandler(m, ackIdx);
  memcpy(&m->round, info->srcAck, sizeof(int));
  CmiSyncSendAndFree(coordinator(), sizeof(RoundMsg), m);
}

// coordinator: one put has completed
static void ackHandler(void *msg) {
  int round = ((RoundMsg *)msg)->round;
  CmiFree(msg);
  if (++CsvAccess(acks) < CmiMyNodeSize() * CmiNodeSize(0))
    return;
  CsvAccess(srcBuf).deregisterMem();
  for (int t = 0; t < CmiNodeSize(0); t++) {
    RoundMsg *m = (RoundMsg *)CmiAlloc(sizeof(RoundMsg));
    CmiSetHandler(m, doneIdx);
    m->round = round;
    CmiSyncSendAndFree(CmiNodeFirst(0) + t, sizeof(RoundMsg), m);
  }
}

// process 0: every put of this round has landed in this PE's destination
static void doneHandler(void *msg) {
  int round = ((RoundMsg *)msg)->round;
  CmiFree(msg);
  for (size_t i = 0; i < kBytes; i++)
    if (CpvAccess(dst)[i] != pattern(round, i))
      CmiAbort("rdma_shared_buffer: PE %d round %d: byte %zu is %d, "
               "expected %d\n", CmiMyPe(), round, i, CpvAccess(dst)[i],
               pattern(round, i));
  CpvAccess(dstBuf).deregisterMem();
  if (round + 1 < rounds) {
    startRound(round + 1);
    return;
  }
  RoundMsg *m = (RoundMsg *)CmiAlloc(sizeof(RoundMsg));
  CmiSetHandler(m, finishIdx);
  m->round = round;
  CmiSyncSendAndFree(0, sizeof(RoundMsg), m);
}

static void finishHandler(void *msg) {
  CmiFree(msg);
  if (++CpvAccess(finished) < CmiNodeSize(0))
    return;
  CmiPrintf("rdma_shared_buffer: %d PEs, %d rounds: every put PASS\n",
            CmiNumPes(), rounds);
  CmiExit(0);
}

static void mymain(int argc, char **argv) {
  targetIdx = CmiRegisterHandler(targetHandler);
  putIdx = CmiRegisterHandler(putHandler);
  ackIdx = CmiRegisterHandler(ackHandler);
  doneIdx = CmiRegisterHandler(doneHandler);
  finishIdx = CmiRegisterHandler(finishHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1)
    rounds = atoi(argv[1]);
  // Peers in the same process would take CmiIssueRput's memcpy shortcut.
  if (CmiNumNodes() != 2 || CmiMyNodeSize() > kMaxPes)
    CmiAbort("rdma_shared_buffer: needs exactly 2 processes of at most %d "
             "PEs, so that every transfer takes the network RDMA path\n",
             kMaxPes);

  CpvInitialize(char *, dst);
  CpvInitialize(CmiNcpyBuffer, dstBuf);
  CpvInitialize(int, finished);
  CpvAccess(finished) = 0;
  if (CmiMyNode() == 0) {
    CpvAccess(dst) = (char *)CmiAlloc(kBytes);
  } else if (CmiMyRank() == 0) {
    CsvInitialize(char *, src);
    CsvInitialize(CmiNcpyBuffer, srcBuf);
    CsvInitialize(PutMsg *, pending);
    CsvInitialize(int, targetsIn);
    CsvInitialize(int, acks);
    CsvAccess(src) = (char *)CmiAlloc(kBytes);
    CsvAccess(pending) = new PutMsg;
    CsvAccess(targetsIn) = 0;
    CsvAccess(acks) = 0;
  }
  CmiSetDirectNcpyAckHandler(ncpyAckHandler);

  // every PE of both processes registers its handlers before anyone sends
  CmiBarrier();

  if (CmiMyNode() == 0)
    startRound(0);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
