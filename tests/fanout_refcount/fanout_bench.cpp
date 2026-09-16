// Fan-out microbenchmark for issue #219: what sending one message to every
// other PE costs, per destination PE (what a list send used to do) versus per
// destination process (what CmiSyncListSendAndFree does now).
//
// Both variants run in the same binary, so "before" and "after" come from one
// run on one machine:
//   per-PE      : the old loop, CmiSyncSend to each destination then CmiFree.
//   per-process : CmiSyncListSendAndFree.
//
// Reported per size and variant:
//   issue us/fanout : time the sending PE spends inside the fan-out itself --
//                     the allocations, the memcpys and the issueAm calls.
//   wall  us/fanout : fan-out until the last destination has acked, so it also
//                     carries the network round trip.
// Plus, derived from the process layout (not instrumented): network sends per
// fan-out, and bytes the sender copies per fan-out.
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
static const char *variantName[2] = {"per-PE     ", "per-process"};

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
static void finish(void);

static void doFanout(void) {
  int size = sizes[curSizeIdx];
  double t0 = CmiWallTimer();
  void *m = makeMsg(curVariant, size, curIter);
  if (curVariant == 0) {
    // What a list send used to be: one allocation, one memcpy and one send per
    // destination PE.
    for (int i = 0; i < numDests; i++)
      CmiSyncSend(dests[i], size, m);
    CmiFree(m);
  } else {
    CmiSyncListSendAndFree(numDests, dests, size, m);
  }
  issueAccum += CmiWallTimer() - t0;
}

static void recv_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  int variant = m->variant, iter = m->iter; // read only
  CmiFree(m);
  Msg *ack = (Msg *)CmiAlloc(sizeof(Msg));
  ack->header.messageSize = sizeof(Msg);
  CmiSetHandler(ack, CpvAccess(ackIdx));
  ack->variant = variant;
  ack->size = 0;
  ack->iter = iter;
  CmiSyncSendAndFree(0, sizeof(Msg), ack);
}

// Network sends and sender-side copies per fan-out, worked out from the
// process layout the same way CmiSyncListSend groups destinations.
static void costModel(int size, int variant, int *sends, long *bytes) {
  if (variant == 0) {
    int remote = 0;
    for (int i = 0; i < numDests; i++)
      if (CmiNodeOf(dests[i]) != CmiMyNode())
        remote++;
    *sends = remote;              // one active message per destination PE
    *bytes = (long)numDests * size; // CmiSyncSend copies for every destination
    return;
  }
  int nodes = CmiNumNodes();
  int nsends = 0;
  long nbytes = 0;
  for (int n = 0; n < nodes; n++) {
    int ranks = 0;
    for (int i = 0; i < numDests; i++)
      if (CmiNodeOf(dests[i]) == n)
        ranks++;
    if (ranks == 0)
      continue;
    if (n == CmiMyNode()) {
      nbytes += (long)ranks * size; // local path is unchanged: a copy per PE
    } else {
      nsends += 1;         // one fan-out message for the whole process
      nbytes += size;      // one memcpy of the payload into it
    }
  }
  *sends = nsends;
  *bytes = nbytes;
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
  CmiPrintf("\n[0] fan-out to %d destination PEs over %d processes, %d "
            "iterations\n",
            numDests, CmiNumNodes(), iters);
  CmiPrintf("[0] size   variant      sends  sender KB   issue us   wall us\n");
  for (int v = 0; v < 2; v++)
    for (int s = 0; s < numSizes; s++) {
      int sends;
      long bytes;
      costModel(sizes[s], v, &sends, &bytes);
      CmiPrintf("[0] %6d %s %6d %10.1f %10.2f %9.2f\n", sizes[s],
                variantName[v], sends, bytes / 1024.0, results[v][s][0],
                results[v][s][1]);
    }
  for (int s = 0; s < numSizes; s++)
    CmiPrintf("[0] %6d B issue ratio old/new %.2fx, wall ratio %.2fx\n",
              sizes[s], results[0][s][0] / results[1][s][0],
              results[0][s][1] / results[1][s][1]);
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
