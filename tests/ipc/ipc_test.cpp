// Shared-memory IPC between processes that share a host (+ipc).
//
// Every PE sends, to every PE outside its own process, one message of each of
// four sizes -- three under the pool's block cutoff, which go through shared
// memory when +ipc is on, and one over it, which must fall back to the
// network. Every PE also sends one node-queue message to each peer process.
//
// Each delivery checks that the payload survived, that the message names this
// PE (a message off the pool has to be indistinguishable from one off the
// network, header included), and acknowledges to PE 0. PE 0 exits everyone
// once every expected delivery has been acknowledged, so a message that is
// lost, duplicated or misrouted hangs or aborts the test rather than passing.
//
// The run is correct whether or not IPC is on; what changes is the counters,
// which are checked against the mode the runtime reports.
#include "converse.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Sizes below the cutoff. The fourth size is computed at run time from the
// cutoff itself, so the fallback case is exact rather than a guess.
static const int kSmallSizes[3] = {64, 1024, 8192};
static const int kNumSizes = 4;

struct Msg {
  CmiMessageHeader header;
  int srcPe;     // sender, so the receiver can rebuild the payload pattern
  int size;      // total message size, which the pattern also depends on
  int isNodeMsg; // whether this went to the node queue
  char payload[1];
};

CpvDeclare(int, p2pIdx);
CpvDeclare(int, nodeIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);

// PE 0 only
static int acksExpected;
static int acksReceived;

// how many messages this PE put through the pool, as counted before it sent
// anything of its own (acks and bootstrap traffic are not ours to predict)
CpvDeclare(long, ipcSentBefore);

static int bigSize(void) {
  // One block larger than the pool can carry. CmiRecommendedIpcBlockCutoff is
  // only meaningful once the pool exists; without it any size will do, since
  // nothing takes the pool path anyway.
  if (CmiIpcEnabled())
    return (int)CmiRecommendedIpcBlockCutoff() + 1024;
  return 512 * 1024;
}

static int sizeFor(int which) {
  return which < 3 ? kSmallSizes[which] : bigSize();
}

static char patternByte(int srcPe, int size, int i) {
  return (char)((srcPe * 31 + size * 17 + i) & 0xff);
}

static int payloadBytes(int size) {
  return size - (int)(sizeof(Msg) - 1);
}

static Msg *makeMsg(int size, int isNodeMsg, int handlerIdx) {
  Msg *m = (Msg *)CmiAlloc(size);
  m->header.messageSize = size;
  CmiSetHandler(m, handlerIdx);
  m->srcPe = CmiMyPe();
  m->size = size;
  m->isNodeMsg = isNodeMsg;
  const int n = payloadBytes(size);
  for (int i = 0; i < n; i++)
    m->payload[i] = patternByte(CmiMyPe(), size, i);
  return m;
}

static void checkMsg(Msg *m, int isNodeMsg) {
  if (m->size < (int)sizeof(Msg) || m->isNodeMsg != isNodeMsg)
    CmiAbort("ipc: message header corrupted (size %d, isNodeMsg %d)", m->size,
             m->isNodeMsg);
  if (m->srcPe < 0 || m->srcPe >= CmiNumPes())
    CmiAbort("ipc: message claims to come from PE %d", m->srcPe);
  if (CmiNodeOf(m->srcPe) == CmiMyNode())
    CmiAbort("ipc: PE %d received a message from its own process (PE %d)",
             CmiMyPe(), m->srcPe);
  const int n = payloadBytes(m->size);
  for (int i = 0; i < n; i++)
    if (m->payload[i] != patternByte(m->srcPe, m->size, i))
      CmiAbort("ipc: payload from PE %d corrupted at byte %d of %d", m->srcPe,
               i, n);
}

static void ackTo0(Msg *m) {
  // Acknowledge with a fixed small message: the original may be a pool block
  // owned by this process, and forwarding it would send that block's address
  // to a process that cannot free it.
  Msg *ack = (Msg *)CmiAlloc(sizeof(Msg));
  ack->header.messageSize = sizeof(Msg);
  CmiSetHandler(ack, CpvAccess(ackIdx));
  ack->srcPe = CmiMyPe();
  ack->size = sizeof(Msg);
  ack->isNodeMsg = 0;
  CmiSyncSendAndFree(0, sizeof(Msg), ack);
  CmiFree(m);
}

static void p2pHandler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  checkMsg(m, 0);
  // A message that came through the pool must name its destination exactly as
  // one off the network does: the pool routes on a rank internally, and this
  // is where that substitution would show through.
  if ((int)m->header.destPE != CmiMyPe())
    CmiAbort("ipc: PE %d got a message addressed to %u", CmiMyPe(),
             (unsigned)m->header.destPE);
  ackTo0(m);
}

static void nodeHandler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  checkMsg(m, 1);
  if (m->header.destPE != CmiMessageDestPENode)
    CmiAbort("ipc: node message arrived addressed to PE %u instead of the "
             "node queue",
             (unsigned)m->header.destPE);
  ackTo0(m);
}

static void exitHandler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void reportAndExit(void) {
  const long sent = CmiIpcMessagesSent() - CpvAccess(ipcSentBefore);
  CmiPrintf("ipc: mode=%s enabled=%d peersOnHost=%d\n", CmiIpcImplName(),
            CmiIpcEnabled(), CmiIpcNumPeers());
  CmiPrintf("ipc: %d deliveries acknowledged, PE 0 put %ld messages through "
            "the pool\n",
            acksReceived, sent);

  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  CmiSetHandler(m, CpvAccess(exitIdx));
  m->srcPe = CmiMyPe();
  m->size = sizeof(Msg);
  m->isNodeMsg = 0;
  CmiSyncBroadcastAllAndFree(sizeof(Msg), m);
}

static void ackHandler(void *vmsg) {
  CmiFree(vmsg);
  if (++acksReceived == acksExpected)
    reportAndExit();
}

// Counts, over the whole job, how many deliveries the send loop below will
// produce. PE 0 waits for exactly this many acknowledgements.
static int expectedDeliveries(void) {
  const int nPes = CmiNumPes();
  const int nNodes = CmiNumNodes();
  const int nodeSize = CmiMyNodeSize();
  const int p2p = nPes * (nPes - nodeSize) * kNumSizes;
  const int node = nPes * (nNodes - 1);
  return p2p + node;
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, p2pIdx);
  CpvInitialize(int, nodeIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvInitialize(long, ipcSentBefore);
  CpvAccess(p2pIdx) = CmiRegisterHandler(p2pHandler);
  CpvAccess(nodeIdx) = CmiRegisterHandler(nodeHandler);
  CpvAccess(ackIdx) = CmiRegisterHandler(ackHandler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exitHandler);

  if (CmiMyPe() == 0) {
    acksExpected = expectedDeliveries();
    acksReceived = 0;
    CmiPrintf("ipc: %d PEs in %d processes, %d deliveries expected, IPC %s\n",
              CmiNumPes(), CmiNumNodes(), acksExpected,
              CmiIpcEnabled() ? CmiIpcImplName() : "off");
  }
  // Everyone has to have registered handlers and PE 0 has to have its counter
  // before the first message goes out.
  CmiBarrier();

  CpvAccess(ipcSentBefore) = CmiIpcMessagesSent();

  // One message of each size to every PE outside this process.
  for (int pe = 0; pe < CmiNumPes(); pe++) {
    if (CmiNodeOf(pe) == CmiMyNode())
      continue;
    for (int which = 0; which < kNumSizes; which++) {
      const int size = sizeFor(which);
      Msg *m = makeMsg(size, 0, CpvAccess(p2pIdx));
      CmiSyncSendAndFree(pe, size, m);
    }
  }

  // One node-queue message to every other process.
  for (int node = 0; node < CmiNumNodes(); node++) {
    if (node == CmiMyNode())
      continue;
    const int size = kSmallSizes[1];
    Msg *m = makeMsg(size, 1, CpvAccess(nodeIdx));
    CmiSyncNodeSendAndFree(node, size, m);
  }

  const long sent = CmiIpcMessagesSent() - CpvAccess(ipcSentBefore);
  if (CmiIpcEnabled()) {
    // Only a run whose host actually holds more than one process can put
    // anything through the pool.
    int peersHere = 0;
    for (int node = 0; node < CmiNumNodes(); node++)
      if (node != CmiMyNode() &&
          CmiPeOnSamePhysicalNode(CmiMyPe(), CmiNodeFirst(node)))
        peersHere++;
    if (peersHere > 0 && sent == 0)
      CmiAbort("ipc: PE %d shares its host with %d other processes and IPC "
               "reports itself enabled, but no message went through the pool",
               CmiMyPe(), peersHere);
    if (peersHere > 0)
      CmiPrintf("ipc: PE %d sent %ld of its messages through the pool (%d "
                "peer processes on this host)\n",
                CmiMyPe(), sent, peersHere);
  } else if (sent != 0) {
    CmiAbort("ipc: PE %d put %ld messages through a pool that is off",
             CmiMyPe(), sent);
  }

  if (CmiMyPe() == 0 && acksExpected == 0)
    reportAndExit();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
