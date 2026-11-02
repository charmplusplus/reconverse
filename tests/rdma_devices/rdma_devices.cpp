// Zero-copy get and put between processes with several LCI devices (#203).
// Two processes of R PEs each. Every PE gets a buffer from, and puts a buffer
// into, every PE of the other process, so buffer owners and initiators meet on
// every combination of devices: with +lci_ndevices 2 and R = 4, ranks 0-1 use
// device 0 and ranks 2-3 device 1. The bytes of every transfer are checked.
//
// Usage: <launcher> -n 2 ./reconverse_rdma_devices +pe 2R [+lci_ndevices D]
//        [bytes per buffer, default 65536]
#include "conv-rdma.h"
#include <converse.h>
#include <stdlib.h>
#include <string.h>

enum { kGet = 1, kPut = 2 };

// Carried as the operation's ack, to route its completion back to the
// initiator: the ack handler runs on whichever thread polled the device.
struct Ack {
  int kind;
  int initiator;
  int peer;
};

struct ExchangeMsg {
  char core[CmiMsgHeaderSizeBytes];
  int from;
  CmiNcpyBuffer src;    // for the receiver to get from
  CmiNcpyBuffer putDst; // for the receiver to put into
};

struct AckMsg {
  char core[CmiMsgHeaderSizeBytes];
  Ack ack;
};

struct PeerMsg {
  char core[CmiMsgHeaderSizeBytes];
  int from;
};

static int exchangeIdx, ackIdx, putLandedIdx, doneIdx;
static size_t bytes = 65536;

// indexed by the peer's rank in the other process
CpvStaticDeclare(char *, src);
CpvStaticDeclare(CmiNcpyBuffer, srcBuf);
CpvStaticDeclare(char **, getDst);
CpvStaticDeclare(CmiNcpyBuffer *, getDstBuf);
CpvStaticDeclare(char **, putDst);
CpvStaticDeclare(int, getsDone);
CpvStaticDeclare(int, putsDone);
CpvStaticDeclare(int, putsLanded);
CpvStaticDeclare(int, pesDone);

static char pattern(int owner, size_t i) {
  return (char)((owner * 31 + i) & 0xff);
}

static void check(const char *what, const char *buf, int owner) {
  for (size_t i = 0; i < bytes; i++)
    if (buf[i] != pattern(owner, i))
      CmiAbort("rdma_devices: PE %d: %s from PE %d: byte %zu is %d, expected "
               "%d\n", CmiMyPe(), what, owner, i, buf[i], pattern(owner, i));
}

static void checkDone() {
  int r = CmiMyNodeSize();
  if (CpvAccess(getsDone) < r || CpvAccess(putsDone) < r ||
      CpvAccess(putsLanded) < r)
    return;
  PeerMsg *m = (PeerMsg *)CmiAlloc(sizeof(PeerMsg));
  CmiSetHandler(m, doneIdx);
  m->from = CmiMyPe();
  CmiSyncSendAndFree(0, sizeof(PeerMsg), m);
}

static void exchangeHandler(void *msg) {
  ExchangeMsg *m = (ExchangeMsg *)msg;
  int peer = CmiRankOf(m->from);
  Ack get = {kGet, CmiMyPe(), m->from};
  CpvAccess(getDstBuf)[peer] =
      CmiNcpyBuffer(CpvAccess(getDst)[peer], bytes);
  CpvAccess(getDstBuf)[peer].rdmaGet(m->src, sizeof(Ack), (char *)&get,
                                     (char *)&get);
  Ack put = {kPut, CmiMyPe(), m->from};
  CpvAccess(srcBuf).rdmaPut(m->putDst, sizeof(Ack), (char *)&put,
                            (char *)&put);
  CmiFree(msg);
}

// Runs on the thread that polled the completion, possibly not the initiator.
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
  int peer = CmiRankOf(ack.peer);
  if (ack.kind == kGet) {
    check("get", CpvAccess(getDst)[peer], ack.peer);
    CpvAccess(getDstBuf)[peer].deregisterMem();
    CpvAccess(getsDone)++;
  } else {
    // the put has completed here; tell the destination to look at it
    CpvAccess(putsDone)++;
    PeerMsg *m = (PeerMsg *)CmiAlloc(sizeof(PeerMsg));
    CmiSetHandler(m, putLandedIdx);
    m->from = CmiMyPe();
    CmiSyncSendAndFree(ack.peer, sizeof(PeerMsg), m);
  }
  checkDone();
}

static void putLandedHandler(void *msg) {
  int from = ((PeerMsg *)msg)->from;
  CmiFree(msg);
  check("put", CpvAccess(putDst)[CmiRankOf(from)], from);
  CpvAccess(putsLanded)++;
  checkDone();
}

static void doneHandler(void *msg) {
  CmiFree(msg);
  if (++CpvAccess(pesDone) < CmiNumPes())
    return;
  CmiPrintf("rdma_devices: %d PEs, %zu bytes per buffer: every get and put "
            "PASS\n", CmiNumPes(), bytes);
  CmiExit(0);
}

static void mymain(int argc, char **argv) {
  exchangeIdx = CmiRegisterHandler(exchangeHandler);
  ackIdx = CmiRegisterHandler(ackHandler);
  putLandedIdx = CmiRegisterHandler(putLandedHandler);
  doneIdx = CmiRegisterHandler(doneHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1)
    bytes = (size_t)atol(argv[1]);
  // Peers in the same process would take CmiIssueRget's memcpy shortcut.
  if (CmiNumNodes() != 2)
    CmiAbort("rdma_devices: needs exactly 2 processes, so that every "
             "transfer takes the network RDMA path\n");

  int r = CmiMyNodeSize(), me = CmiMyPe();
  CpvInitialize(char *, src);
  CpvInitialize(CmiNcpyBuffer, srcBuf);
  CpvInitialize(char **, getDst);
  CpvInitialize(CmiNcpyBuffer *, getDstBuf);
  CpvInitialize(char **, putDst);
  CpvInitialize(int, getsDone);
  CpvInitialize(int, putsDone);
  CpvInitialize(int, putsLanded);
  CpvInitialize(int, pesDone);
  CpvAccess(getsDone) = CpvAccess(putsDone) = CpvAccess(putsLanded) = 0;
  CpvAccess(pesDone) = 0;

  CpvAccess(src) = (char *)CmiAlloc(bytes);
  for (size_t i = 0; i < bytes; i++)
    CpvAccess(src)[i] = pattern(me, i);
  CpvAccess(srcBuf) = CmiNcpyBuffer(CpvAccess(src), bytes);
  CpvAccess(getDst) = new char *[r];
  CpvAccess(getDstBuf) = new CmiNcpyBuffer[r];
  CpvAccess(putDst) = new char *[r];
  for (int i = 0; i < r; i++) {
    CpvAccess(getDst)[i] = (char *)CmiAlloc(bytes);
    CpvAccess(putDst)[i] = (char *)CmiAlloc(bytes);
    memset(CpvAccess(getDst)[i], 0, bytes);
    memset(CpvAccess(putDst)[i], 0, bytes);
  }
  CmiSetDirectNcpyAckHandler(ncpyAckHandler);

  // every PE of both processes registers its handlers before anyone sends
  CmiBarrier();

  int otherFirst = CmiNodeFirst(1 - CmiMyNode());
  for (int i = 0; i < r; i++) {
    ExchangeMsg *m = (ExchangeMsg *)CmiAlloc(sizeof(ExchangeMsg));
    CmiSetHandler(m, exchangeIdx);
    m->from = me;
    m->src = CpvAccess(srcBuf);
    // the buffer that PE otherFirst + i puts into; it stays registered until
    // exit, which deregisters whatever is left
    m->putDst = CmiNcpyBuffer(CpvAccess(putDst)[i], bytes);
    CmiSyncSendAndFree(otherFirst + i, sizeof(ExchangeMsg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
