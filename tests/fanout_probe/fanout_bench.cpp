// Fan-out microbenchmark for reconverse issue #219.
//
// PE 0 fans one message out to every other PE (7 destinations on a
// lcrun -n 4 ... +pe 8 run) for a fixed number of iterations, two ways:
//
//   copy  : CmiSyncListSendAndFree as it is today - one CmiAlloc + memcpy +
//           issueAm per destination.
//   share : one buffer, one CmiReference per destination, one
//           CmiSyncSendAndFree per destination from that same buffer, then the
//           caller's CmiFree. This is the shape issue #219 proposes. It is
//           MEASUREMENT ONLY: CmiSyncSendAndFree stamps header->destPE into the
//           shared buffer before each issueAm, so with more than one destination
//           rank per remote node the messages mis-deliver (see fanout_probe).
//
// Reported per size and variant:
//   issue us/fanout : time PE 0 spends inside the fan-out call (the part the
//                     per-destination alloc+memcpy pays for)
//   wall  us/fanout : fan-out to last ack, i.e. including the network RTT
//
// Run: lcrun -n 4 ./reconverse_fanout_bench +pe 8 [iters]
#include "converse.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Msg {
  CmiMessageHeader header;
  int variant;
  int size;
  int iter;
};

CpvDeclare(int, recvIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);

static int iters = 1000;
static const int sizes[] = {4096, 65536};
static const int numSizes = 2;
static const char *variantName[2] = {"copy ", "share"};

// PE 0 state
static int *dests = nullptr;
static int numDests = 0;
static int curVariant = 0;
static int curSizeIdx = 0;
static int curIter = 0;
static int acksThisIter = 0;
static double issueAccum = 0.0;
static double wallStart = 0.0;
static double results[2][2][2]; // [variant][sizeIdx][0=issue us,1=wall us]

static void *makeMsg(int variant, int size, int iter) {
  Msg *m = (Msg *)CmiAlloc(size);
  m->header.messageSize = size;
  CmiSetHandler(m, CpvAccess(recvIdx));
  m->variant = variant;
  m->size = size;
  m->iter = iter;
  return m;
}

static void startBatch(void);
static void nextIter(void);
static void finish(void);

static void doFanout(void) {
  int size = sizes[curSizeIdx];
  double t0 = CmiWallTimer();
  if (curVariant == 0) {
    void *m = makeMsg(0, size, curIter);
    CmiSyncListSendAndFree(numDests, dests, size, m);
  } else {
    void *m = makeMsg(1, size, curIter);
    for (int i = 0; i < numDests; i++)
      CmiReference(m); // one reference per destination
    for (int i = 0; i < numDests; i++)
      CmiSyncSendAndFree(dests[i], size, m);
    CmiFree(m); // drop the caller's own reference (AndFree contract)
  }
  issueAccum += CmiWallTimer() - t0;
}

static void recv_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  int variant = m->variant, iter = m->iter; // read only; never write a shared msg
  CmiFree(m);
  Msg *ack = (Msg *)CmiAlloc(sizeof(Msg));
  ack->header.messageSize = sizeof(Msg);
  CmiSetHandler(ack, CpvAccess(ackIdx));
  ack->variant = variant;
  ack->size = 0;
  ack->iter = iter;
  CmiSyncSendAndFree(0, sizeof(Msg), ack);
}

static void ack_handler(void *vmsg) {
  CmiFree(vmsg);
  if (++acksThisIter < numDests)
    return;
  acksThisIter = 0;
  if (++curIter < iters) {
    doFanout();
    return;
  }
  double wall = CmiWallTimer() - wallStart;
  results[curVariant][curSizeIdx][0] = issueAccum / iters * 1e6;
  results[curVariant][curSizeIdx][1] = wall / iters * 1e6;
  CmiPrintf("[0] %s %6d B: issue %8.2f us/fanout, wall %8.2f us/fanout\n",
            variantName[curVariant], sizes[curSizeIdx],
            results[curVariant][curSizeIdx][0],
            results[curVariant][curSizeIdx][1]);
  if (++curSizeIdx < numSizes) {
    startBatch();
    return;
  }
  curSizeIdx = 0;
  if (++curVariant < 2) {
    startBatch();
    return;
  }
  CmiPrintf("\n[0] fan-out to %d destinations, %d iterations, %d processes\n",
            numDests, iters, CmiNumNodes());
  CmiPrintf("[0] size    copy issue  share issue   ratio | copy wall  share "
            "wall   ratio\n");
  for (int s = 0; s < numSizes; s++)
    CmiPrintf("[0] %6d %11.2f %12.2f %7.2fx %10.2f %11.2f %7.2fx\n", sizes[s],
              results[0][s][0], results[1][s][0],
              results[0][s][0] / results[1][s][0], results[0][s][1],
              results[1][s][1], results[0][s][1] / results[1][s][1]);
  finish();
}

static void startBatch(void) {
  curIter = 0;
  acksThisIter = 0;
  issueAccum = 0.0;
  wallStart = CmiWallTimer();
  doFanout();
}

static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void finish(void) {
  Msg *e = (Msg *)CmiAlloc(sizeof(Msg));
  e->header.messageSize = sizeof(Msg);
  CmiSetHandler(e, CpvAccess(exitIdx));
  CmiSyncBroadcastAllAndFree(sizeof(Msg), e);
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, recvIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvAccess(recvIdx) = CmiRegisterHandler(recv_handler);
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  if (argc > 1)
    iters = atoi(argv[1]);
  if (CmiMyPe() != 0)
    return;
  numDests = CmiNumPes() - 1;
  if (numDests < 1) {
    CmiPrintf("fanout_bench needs more than one PE\n");
    finish();
    return;
  }
  dests = new int[numDests];
  for (int i = 0; i < numDests; i++)
    dests[i] = i + 1;
  startBatch();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
