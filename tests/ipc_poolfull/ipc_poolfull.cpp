/* ipc_poolfull: what happens when a sender has used up the destination's
 * segment.
 *
 * A freed block goes back only to the PE that allocated it, and blocks never
 * return to the segment's unused space. So once that space is used up, a
 * sender with no freed block of the right size sends over the network, and
 * CmiIpcPoolFullSends counts it. This test makes that happen on purpose:
 *
 *  1. PE 0 sends a burst of BURST messages to the first PE of process 1,
 *     which holds every message, without freeing any, until the whole burst
 *     has arrived. The segment (++ipcpoolsize) holds far fewer, so some must
 *     go over the network. Every message of the burst either went through
 *     the pool or was counted as a full-segment send, and nothing is lost.
 *  2. The receiver frees the burst and reports. PE 0 sends a second burst:
 *     its freed blocks have come back to it, so exactly as many messages as
 *     in the first burst go through the pool again, and the rest are counted
 *     as full-segment sends again.
 *
 * Run with 2 processes, +ipc and a small ++ipcpoolsize; other PEs only wait.
 */
#include <converse.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#define BURST 400
#define PAYLOAD 1000

struct DataMsg {
  CmiMessageHeader header;
  int round;
  int seq;
  char payload[PAYLOAD];
};

struct ReportMsg {
  CmiMessageHeader header;
  int round;
  long poolBytesUsed;   // CmiIpcPoolBytesUsed on the receiver
};

CpvDeclare(int, dataIdx);
CpvDeclare(int, reportIdx);
CpvDeclare(int, exitIdx);

// receiver (first PE of process 1)
static std::vector<DataMsg*> held;

// sender (PE 0): pool sends and full-segment sends of round 1
static long poolSent1;
static long full1;

static int receiverPe(void) { return CmiNodeFirst(1); }

static char fill(int round, int seq, int i) {
  return (char)((round * 131 + seq * 31 + i) & 0xff);
}

static void exitHandler(void* msg) {
  CmiFree(msg);
  CsdExitScheduler();
}

static void exitAll(void) {
  auto* m = (CmiMessageHeader*)CmiAlloc(sizeof(CmiMessageHeader));
  CmiSetHandler(m, CpvAccess(exitIdx));
  CmiSyncBroadcastAllAndFree(sizeof(CmiMessageHeader), m);
}

// sends one burst and returns (pool sends, full-segment sends) during it
static void sendBurst(int round, long* poolSent, long* full) {
  const long sentBefore = CmiIpcMessagesSent();
  const long fullBefore = CmiIpcPoolFullSends();
  for (int seq = 0; seq < BURST; seq++) {
    auto* m = (DataMsg*)CmiAlloc(sizeof(DataMsg));
    CmiSetHandler(m, CpvAccess(dataIdx));
    m->round = round;
    m->seq = seq;
    for (int i = 0; i < PAYLOAD; i++) m->payload[i] = fill(round, seq, i);
    CmiSyncSendAndFree(receiverPe(), sizeof(DataMsg), m);
  }
  *poolSent = CmiIpcMessagesSent() - sentBefore;
  *full = CmiIpcPoolFullSends() - fullBefore;
  CmiPrintf("ipc_poolfull: round %d: %ld of %d messages through the pool, "
            "%ld over the network because the segment was full\n",
            round, *poolSent, BURST, *full);
  if (*poolSent + *full != BURST)
    CmiAbort("ipc_poolfull: round %d: %ld pool sends and %ld full-segment "
             "sends do not add up to the burst of %d",
             round, *poolSent, *full, BURST);
  if (*full == 0)
    CmiAbort("ipc_poolfull: round %d: the segment never filled; raise BURST "
             "or lower ++ipcpoolsize", round);
}

static void dataHandler(void* vmsg) {
  auto* m = (DataMsg*)vmsg;
  for (int i = 0; i < PAYLOAD; i++)
    if (m->payload[i] != fill(m->round, m->seq, i))
      CmiAbort("ipc_poolfull: round %d message %d corrupted at byte %d",
               m->round, m->seq, i);
  held.push_back(m);
  if ((int)held.size() < BURST) return;
  // the whole burst is here: free it, which returns the pool blocks to PE 0
  const int round = m->round;
  std::vector<char> seen(BURST, 0);
  for (auto* h : held) {
    if (h->round != round || h->seq < 0 || h->seq >= BURST || seen[h->seq]++)
      CmiAbort("ipc_poolfull: unexpected or duplicate message (round %d, "
               "seq %d)", h->round, h->seq);
    CmiFree(h);
  }
  held.clear();
  auto* r = (ReportMsg*)CmiAlloc(sizeof(ReportMsg));
  CmiSetHandler(r, CpvAccess(reportIdx));
  r->round = round;
  r->poolBytesUsed = (long)CmiIpcPoolBytesUsed();
  CmiSyncSendAndFree(0, sizeof(ReportMsg), r);
}

static void reportHandler(void* vmsg) {
  auto* r = (ReportMsg*)vmsg;
  const int round = r->round;
  CmiPrintf("ipc_poolfull: round %d delivered; %ld bytes of process 1's "
            "segment are in blocks\n", round, r->poolBytesUsed);
  if (r->poolBytesUsed <= 0)
    CmiAbort("ipc_poolfull: process 1 reports no segment space in use");
  CmiFree(r);
  if (round == 1) {
    long poolSent2, full2;
    sendBurst(2, &poolSent2, &full2);
    // the blocks of round 1 came back to PE 0, and only to PE 0
    if (poolSent2 != poolSent1)
      CmiAbort("ipc_poolfull: round 2 put %ld messages through the pool, "
               "round 1 put %ld; PE 0's freed blocks did not all come back",
               poolSent2, poolSent1);
  } else {
    CmiPrintf("ipc_poolfull: passed\n");
    exitAll();
  }
}

static void mymain(int argc, char** argv) {
  CpvInitialize(int, dataIdx);
  CpvInitialize(int, reportIdx);
  CpvInitialize(int, exitIdx);
  CpvAccess(dataIdx) = CmiRegisterHandler(dataHandler);
  CpvAccess(reportIdx) = CmiRegisterHandler(reportHandler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exitHandler);
  if (CmiNumNodes() != 2)
    CmiAbort("ipc_poolfull: run with exactly 2 processes (got %d)",
             CmiNumNodes());
  CmiBarrier();
  if (CmiMyPe() != 0) return;
  if (!CmiIpcEnabled()) {
    // nothing to fill: the pool is off or the processes are on two hosts
    CmiPrintf("ipc_poolfull: pool not in use, nothing to test\n");
    exitAll();
    return;
  }
  if (sizeof(DataMsg) > CmiRecommendedIpcBlockCutoff())
    CmiAbort("ipc_poolfull: messages of %zu bytes exceed the cutoff %zu",
             sizeof(DataMsg), CmiRecommendedIpcBlockCutoff());
  sendBurst(1, &poolSent1, &full1);
}

int main(int argc, char** argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
