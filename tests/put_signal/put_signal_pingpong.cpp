// Put-and-notify latency: a ping-pong of puts with signal (CmiIssueRputSignal)
// between PE 0 and the first PE of process 1, against a ping-pong of Converse
// messages of the same payload. Each side's signal handler pushes a message
// onto its PE, whose handler puts back, so a round trip is two puts, two
// signals and two scheduler entries, as an application would see it.
//
// Usage: <launcher> -n 2 ./put_signal_pingpong +pe 2 [iterations, default
//        1000] [warm-up iterations, default 100]
// Prints one line per size: the one-way time of each, in microseconds.
#include "conv-rdma.h"
#include <converse.h>
#include <stdlib.h>
#include <string.h>

static const size_t kSizes[] = {8, 64, 1024, 8192, 65536};
static const int kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);
static const size_t kMaxBytes = 65536;

struct BufMsg {
  char core[CmiMsgHeaderSizeBytes];
  CmiNcpyBuffer dest;
};
struct PingMsg {
  char core[CmiMsgHeaderSizeBytes];
};

static int iterations = 1000, warmup = 100;
static int kind;
static int bufIdx, bounceIdx, msgIdx;

CpvStaticDeclare(int, peer);
CpvStaticDeclare(char *, src);
CpvStaticDeclare(CmiNcpyBuffer, srcReg);
CpvStaticDeclare(char *, dst);
CpvStaticDeclare(CmiNcpyBuffer, dstReg);
CpvStaticDeclare(CmiNcpyBuffer, peerDst);
CpvStaticDeclare(int, sizeIdx);
CpvStaticDeclare(int, iter);
CpvStaticDeclare(double, t0);
CpvStaticDeclare(double, putUs);

static void putTo() {
  size_t n = kSizes[CpvAccess(sizeIdx)];
  CmiNcpyBuffer s = CpvAccess(srcReg);
  s.cnt = n;
  CmiNcpyBuffer d = CpvAccess(peerDst);
  d.cnt = n;
  NcpyOperationInfo *info = s.createNcpyOpInfo(s, d, 0, nullptr, nullptr, -1,
                                               CMK_DIRECT_API, nullptr);
  CmiIssueRputSignal(info, CpvAccess(peer), kind, 1);
}

static void sendMsg() {
  size_t n = kSizes[CpvAccess(sizeIdx)];
  int bytes = (int)(CmiMsgHeaderSizeBytes + n);
  char *m = (char *)CmiAlloc(bytes);
  CmiSetHandler(m, msgIdx);
  CmiSyncSendAndFree(CpvAccess(peer), bytes, m);
}

// The local completion of a put: nothing to do, the next put of this source
// starts only after the peer has seen this one.
static void ncpyAckHandler(void *) {}

// On whatever thread got the signal: hand it to the PE.
static void signalHandler(void *, int destPe, uint16_t signal, int, size_t) {
  PingMsg *m = (PingMsg *)CmiAlloc(sizeof(PingMsg));
  CmiSetHandler(m, bounceIdx);
  CmiPushPE(CmiRankOf(destPe), m);
}

// PE 0 counts round trips; the other PE answers. Returns true when PE 0 has
// finished the current size.
static bool roundTrip() {
  if (CmiMyPe() != 0)
    return false;
  int i = ++CpvAccess(iter);
  if (i == warmup)
    CpvAccess(t0) = CmiWallTimer();
  return i == warmup + iterations;
}

static double oneWayUs() {
  return (CmiWallTimer() - CpvAccess(t0)) * 1e6 / (2.0 * iterations);
}

static void bounceHandler(void *msg) {
  CmiFree(msg);
  if (!roundTrip()) {
    putTo();
    return;
  }
  CpvAccess(putUs) = oneWayUs();
  CpvAccess(iter) = 0;
  sendMsg();  // the message ping-pong of the same size
}

static void msgHandler(void *msg) {
  CmiFree(msg);
  if (!roundTrip()) {
    sendMsg();
    return;
  }
  CmiPrintf("put_signal_pingpong: %6zu bytes: put with signal %8.2f us, "
            "message %8.2f us (one way, %d iterations)\n",
            kSizes[CpvAccess(sizeIdx)], CpvAccess(putUs), oneWayUs(),
            iterations);
  CpvAccess(iter) = 0;
  if (++CpvAccess(sizeIdx) == kNumSizes)
    CmiExit(0);
  else
    putTo();
}

// The other side's destination. PE 0 sends first, so when PE 0 has the
// peer's, both have each other's, and PE 0 starts.
static void bufHandler(void *msg) {
  CpvAccess(peerDst) = ((BufMsg *)msg)->dest;
  CmiFree(msg);
  if (CmiMyPe() == 0) {
    CpvAccess(t0) = CmiWallTimer();
    putTo();
    return;
  }
  BufMsg *m = (BufMsg *)CmiAlloc(sizeof(BufMsg));
  CmiSetHandler(m, bufIdx);
  m->dest = CpvAccess(dstReg);
  CmiSyncSendAndFree(CpvAccess(peer), sizeof(BufMsg), m);
}

static void mymain(int argc, char **argv) {
  bufIdx = CmiRegisterHandler(bufHandler);
  bounceIdx = CmiRegisterHandler(bounceHandler);
  msgIdx = CmiRegisterHandler(msgHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1)
    iterations = atoi(argv[1]);
  if (argc > 2)
    warmup = atoi(argv[2]);
  if (CmiNumNodes() != 2 || iterations < 1 || warmup < 1)
    CmiAbort("put_signal_pingpong: needs 2 processes, and at least one "
             "iteration and one warm-up iteration\n");
  CpvInitialize(int, peer);
  CpvInitialize(char *, src);
  CpvInitialize(CmiNcpyBuffer, srcReg);
  CpvInitialize(char *, dst);
  CpvInitialize(CmiNcpyBuffer, dstReg);
  CpvInitialize(CmiNcpyBuffer, peerDst);
  CpvInitialize(int, sizeIdx);
  CpvInitialize(int, iter);
  CpvInitialize(double, t0);
  CpvInitialize(double, putUs);
  CpvAccess(sizeIdx) = CpvAccess(iter) = 0;
  CmiSetDirectNcpyAckHandler(ncpyAckHandler);
  if (CmiMyRank() == 0) {
    kind = CmiRegisterRputSignalKind(signalHandler, nullptr);
    if (CmiMyPe() == 0)
      CmiPrintf("put_signal_pingpong: CmiIsPutSignalCapable() = %d\n",
                CmiIsPutSignalCapable());
  }
  CmiBarrier();
  int me = CmiMyPe(), other = CmiNodeFirst(1);
  if (me != 0 && me != other)
    return;
  CpvAccess(peer) = me == 0 ? other : 0;
  CpvAccess(src) = (char *)malloc(kMaxBytes);
  memset(CpvAccess(src), 1, kMaxBytes);
  CpvAccess(srcReg) = CmiNcpyBuffer(CpvAccess(src), kMaxBytes);
  CpvAccess(dst) = (char *)malloc(kMaxBytes);
  CpvAccess(dstReg) = CmiNcpyBuffer(CpvAccess(dst), kMaxBytes);
  if (me == 0) {
    BufMsg *m = (BufMsg *)CmiAlloc(sizeof(BufMsg));
    CmiSetHandler(m, bufIdx);
    m->dest = CpvAccess(dstReg);
    CmiSyncSendAndFree(other, sizeof(BufMsg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
