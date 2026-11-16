// Zero-copy gets and puts between two processes with CMK_BUFFER_UNREG and
// CMK_BUFFER_NODEREG on both sides (#331), counting the registrations each
// process holds (CmiRegisteredBuffers).
//
// Every PE pairs with the PE of the same rank in the other process. It owns a
// buffer for the peer to get from and one for the peer to put into, sends the
// peer their handles once, and never registers them: each transfer reaches
// the owner as a registration request. Each PE then runs the same number of
// rounds, one get and one put per round, reusing the same two handles of its
// own. When every PE has finished and released its two handles, no
// registration may be left in either process:
//  - the owner registers its buffer for one transfer (regAndPutHandler,
//    regAndGetHandler); with NODEREG nothing above Converse releases that, so
//    the owner must release it when the transfer completes;
//  - a reused NODEREG handle on the initiating side must be registered once,
//    not again for every transfer.
//
// Usage: <launcher> -n 2 ./rdma_unreg_nodereg +pe 2R [rounds, default 100]
//        [bytes per buffer, default 16384]
#include "conv-rdma.h"
#include <converse.h>
#include <stdlib.h>
#include <string.h>

enum { kGet = 1, kPut = 2 };

// Carried as the operation's ack. The completion of a transfer the owner
// performs runs on the owner's process, so it is routed back to the
// initiator by message.
struct Ack {
  int kind;
  int initiator;
};

struct ExchangeMsg {
  char core[CmiMsgHeaderSizeBytes];
  CmiNcpyBuffer getSrc; // for the peer to get from
  CmiNcpyBuffer putDst; // for the peer to put into
};

struct AckMsg {
  char core[CmiMsgHeaderSizeBytes];
  Ack ack;
};

struct ReportMsg {
  char core[CmiMsgHeaderSizeBytes];
  int pe;
  long registered;  // CmiRegisteredBuffers() in this PE's process
  long afterRound1; // the same, when this PE finished its first round
};

static int exchangeIdx, ackIdx, doneIdx, checkIdx, reportIdx;
static int rounds = 100;
static size_t bytes = 16384;

CpvStaticDeclare(int, peer);
CpvStaticDeclare(char *, getSrc); // owned for the peer
CpvStaticDeclare(char *, putDst); // owned for the peer
CpvStaticDeclare(char *, getDst);
CpvStaticDeclare(char *, putSrc);
CpvStaticDeclare(CmiNcpyBuffer, getDstH);
CpvStaticDeclare(CmiNcpyBuffer, putSrcH);
CpvStaticDeclare(CmiNcpyBuffer, peerGetSrc);
CpvStaticDeclare(CmiNcpyBuffer, peerPutDst);
CpvStaticDeclare(int, round);
CpvStaticDeclare(int, pending);
CpvStaticDeclare(long, afterRound1);
CpvStaticDeclare(int, pesDone);
CpvStaticDeclare(int, reports);

static char pattern(int owner, size_t i) {
  return (char)((owner * 31 + i) & 0xff);
}

static void check(const char *what, const char *buf, int owner) {
  for (size_t i = 0; i < bytes; i++)
    if (buf[i] != pattern(owner, i))
      CmiAbort("rdma_unreg_nodereg: PE %d round %d: %s from PE %d: byte %zu "
               "is %d, expected %d\n", CmiMyPe(), CpvAccess(round), what,
               owner, i, buf[i], pattern(owner, i));
}

static void startRound() {
  memset(CpvAccess(getDst), 0, bytes);
  CpvAccess(pending) = 2;
  Ack get = {kGet, CmiMyPe()};
  CpvAccess(getDstH).rdmaGet(CpvAccess(peerGetSrc), sizeof(Ack), (char *)&get,
                             (char *)&get);
  Ack put = {kPut, CmiMyPe()};
  CpvAccess(putSrcH).rdmaPut(CpvAccess(peerPutDst), sizeof(Ack), (char *)&put,
                             (char *)&put);
}

static void exchangeHandler(void *msg) {
  ExchangeMsg *m = (ExchangeMsg *)msg;
  CpvAccess(peerGetSrc) = m->getSrc;
  CpvAccess(peerPutDst) = m->putDst;
  CmiFree(msg);
  if (CpvAccess(peerGetSrc).isRegistered || CpvAccess(peerPutDst).isRegistered)
    CmiAbort("rdma_unreg_nodereg: the peer's handles are registered\n");
  startRound();
}

// Runs on the thread that polled the completion, in the owner's process.
static void ncpyAckHandler(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;
  AckMsg *m = (AckMsg *)CmiAlloc(sizeof(AckMsg));
  CmiSetHandler(m, ackIdx);
  memcpy(&m->ack, info->srcAck, sizeof(Ack));
  CmiSyncSendAndFree(m->ack.initiator, sizeof(AckMsg), m);
}

static void ackHandler(void *msg) {
  Ack ack = ((AckMsg *)msg)->ack;
  CmiFree(msg);
  if (ack.kind == kGet)
    check("get", CpvAccess(getDst), CpvAccess(peer));
  if (--CpvAccess(pending) > 0)
    return;
  if (++CpvAccess(round) == 1)
    CpvAccess(afterRound1) = CmiRegisteredBuffers();
  if (CpvAccess(round) < rounds) {
    startRound();
    return;
  }
  CpvAccess(getDstH).deregisterMem();
  CpvAccess(putSrcH).deregisterMem();
  void *done = CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(done, doneIdx);
  CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes, done);
}

// On PE 0: every transfer has completed and every handle has been released.
static void doneHandler(void *msg) {
  CmiFree(msg);
  if (++CpvAccess(pesDone) < CmiNumPes())
    return;
  void *m = CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(m, checkIdx);
  CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, m);
}

static void checkHandler(void *msg) {
  CmiFree(msg);
  check("put", CpvAccess(putDst), CpvAccess(peer));
  ReportMsg *r = (ReportMsg *)CmiAlloc(sizeof(ReportMsg));
  CmiSetHandler(r, reportIdx);
  r->pe = CmiMyPe();
  r->registered = CmiRegisteredBuffers();
  r->afterRound1 = CpvAccess(afterRound1);
  CmiSyncSendAndFree(0, sizeof(ReportMsg), r);
}

static void reportHandler(void *msg) {
  ReportMsg *r = (ReportMsg *)msg;
  // A PE's own two handles were registered when it finished round 1, so a
  // count below 2 means nothing was registered at all (copy-based
  // transfers), and the test would prove nothing.
  if (r->afterRound1 < 2)
    CmiAbort("rdma_unreg_nodereg: PE %d saw %ld registrations after round 1; "
             "are transfers copy-based?\n", r->pe, r->afterRound1);
  if (r->registered != 0)
    CmiAbort("rdma_unreg_nodereg: process %d still holds %ld registrations "
             "after %d rounds, with every handle released (%ld after round 1 "
             "on PE %d)\n", CmiNodeOf(r->pe), r->registered, rounds,
             r->afterRound1, r->pe);
  CmiFree(msg);
  if (++CpvAccess(reports) < CmiNumPes())
    return;
  CmiPrintf("rdma_unreg_nodereg: %d PEs, %d rounds of %zu bytes: no "
            "registration left PASS\n", CmiNumPes(), rounds, bytes);
  CmiExit(0);
}

static void mymain(int argc, char **argv) {
  exchangeIdx = CmiRegisterHandler(exchangeHandler);
  ackIdx = CmiRegisterHandler(ackHandler);
  doneIdx = CmiRegisterHandler(doneHandler);
  checkIdx = CmiRegisterHandler(checkHandler);
  reportIdx = CmiRegisterHandler(reportHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1)
    rounds = atoi(argv[1]);
  if (argc > 2)
    bytes = (size_t)atol(argv[2]);
  // Peers in the same process would take the memcpy shortcut.
  if (CmiNumNodes() != 2)
    CmiAbort("rdma_unreg_nodereg: needs exactly 2 processes, so that every "
             "transfer takes the network RDMA path\n");
  if (rounds < 1)
    CmiAbort("rdma_unreg_nodereg: rounds must be at least 1\n");

  int me = CmiMyPe();
  CpvInitialize(int, peer);
  CpvInitialize(char *, getSrc);
  CpvInitialize(char *, putDst);
  CpvInitialize(char *, getDst);
  CpvInitialize(char *, putSrc);
  CpvInitialize(CmiNcpyBuffer, getDstH);
  CpvInitialize(CmiNcpyBuffer, putSrcH);
  CpvInitialize(CmiNcpyBuffer, peerGetSrc);
  CpvInitialize(CmiNcpyBuffer, peerPutDst);
  CpvInitialize(int, round);
  CpvInitialize(int, pending);
  CpvInitialize(long, afterRound1);
  CpvInitialize(int, pesDone);
  CpvInitialize(int, reports);
  CpvAccess(round) = CpvAccess(pending) = 0;
  CpvAccess(afterRound1) = 0;
  CpvAccess(pesDone) = CpvAccess(reports) = 0;
  CpvAccess(peer) = CmiNodeFirst(1 - CmiMyNode()) + CmiMyRank();

  CpvAccess(getSrc) = (char *)malloc(bytes);
  CpvAccess(putSrc) = (char *)malloc(bytes);
  CpvAccess(getDst) = (char *)malloc(bytes);
  CpvAccess(putDst) = (char *)malloc(bytes);
  for (size_t i = 0; i < bytes; i++)
    CpvAccess(getSrc)[i] = CpvAccess(putSrc)[i] = pattern(me, i);
  memset(CpvAccess(putDst), 0, bytes);
  CpvAccess(getDstH) = CmiNcpyBuffer(CpvAccess(getDst), bytes,
                                     CMK_BUFFER_UNREG, CMK_BUFFER_NODEREG);
  CpvAccess(putSrcH) = CmiNcpyBuffer(CpvAccess(putSrc), bytes,
                                     CMK_BUFFER_UNREG, CMK_BUFFER_NODEREG);
  CmiSetDirectNcpyAckHandler(ncpyAckHandler);

  // every PE of both processes registers its handlers before anyone sends
  CmiBarrier();

  ExchangeMsg *m = (ExchangeMsg *)CmiAlloc(sizeof(ExchangeMsg));
  CmiSetHandler(m, exchangeIdx);
  m->getSrc = CmiNcpyBuffer(CpvAccess(getSrc), bytes, CMK_BUFFER_UNREG,
                            CMK_BUFFER_NODEREG);
  m->putDst = CmiNcpyBuffer(CpvAccess(putDst), bytes, CMK_BUFFER_UNREG,
                            CMK_BUFFER_NODEREG);
  CmiSyncSendAndFree(CpvAccess(peer), sizeof(ExchangeMsg), m);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
