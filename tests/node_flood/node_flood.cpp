// A stream of node-level messages must not starve a PE's own work (#268).
//
// The registered scheduler takes node-level messages ahead of the PE's own
// queued work, up to +node_queue_max in a row. Here the node queue never
// empties: a few node messages re-send themselves to this process forever.
// Meanwhile every PE runs a local message that re-sends itself to the PE
// `count` times. The test passes when every PE finishes its local count; if
// node messages could starve local work it would hang instead.
//
// Mode `node` streams through the converse node queue, `nodeprio` through the
// node priority queue (CsdNodeEnqueueGeneral, priority 0, which ties with the
// local messages' priority and so is also taken first).
//
// Usage: node_flood +pe N [count] [node|nodeprio]   (count defaults to 20000)
#include "converse.h"
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int node_idx, local_idx;
static long count = 20000;
static bool viaNodePrio = false;
static std::atomic<long> nodeHandled{0};
static std::atomic<int> pesDone{0};
static thread_local long localDone = 0;

struct Hdr {
  char core[CmiMsgHeaderSizeBytes];
};

static void sendNode(void *msg) {
  if (viaNodePrio) CsdNodeEnqueueGeneral(msg, CQS_QUEUEING_FIFO, 0, NULL);
  else CmiSyncNodeSendAndFree(CmiMyNode(), sizeof(Hdr), msg);
}

static void nodeHandler(void *msg) {
  nodeHandled++;
  sendNode(msg);
}

static void localHandler(void *msg) {
  if (++localDone < count) {
    CmiSyncSendAndFree(CmiMyPe(), sizeof(Hdr), msg);
    return;
  }
  CmiFree(msg);
  if (++pesDone == CmiMyNodeSize()) {
    CmiPrintf("node_flood: %s, %d PEs each ran %ld local messages while "
              "%ld node messages ran\n",
              viaNodePrio ? "nodeprio" : "node", CmiMyNodeSize(), count,
              nodeHandled.load());
    CmiExit(0);
  }
}

static void moduleInit(int argc, char **argv) {
  node_idx = CmiRegisterHandler(nodeHandler);
  local_idx = CmiRegisterHandler(localHandler);
  if (argc > 1) count = atol(argv[1]);
  if (argc > 2) viaNodePrio = strcmp(argv[2], "nodeprio") == 0;
  // every PE registers its handlers before any message is sent
  CmiNodeAllBarrier();
  if (CmiMyRank() == 0)
    for (int i = 0; i < 2 * CmiMyNodeSize(); i++) {
      Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
      CmiSetHandler(m, node_idx);
      sendNode(m);
    }
  Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
  CmiSetHandler(m, local_idx);
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Hdr), m);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
