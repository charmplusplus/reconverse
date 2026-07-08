/***************************************************************
 * Test for CmiEnqueueNodeFifo / CmiEnqueueNodeLifo across nodes.
 *
 * CmiEnqueueNodeFifo/Lifo only push directly onto the local node
 * queue when the destination node is the caller's own node. Any
 * other destination falls through to comm_backend::issueAm (see
 * convcore.cpp), which is the inter-process/network path. This
 * test arranges every node to target a *different* node so that
 * both enqueue functions are forced through issueAm, to catch
 * segfaults on that code path.
 *
 * Each node sends numMsgs CmiEnqueueNodeFifo messages and numMsgs
 * CmiEnqueueNodeLifo messages to the next node in a ring
 * (destNode = (myNode + 1) % numNodes). Receiving handlers may run
 * on any PE of the destination node, since the node fifo/lifo
 * queues are shared per-node. Once a node has received all of its
 * expected messages, it reports in to PE 0 (global); once PE 0
 * hears from every node, it calls CmiExit to shut down cleanly.
 ****************************************************************/

#include "converse.h"
#include <atomic>

int numMsgs = 5;

int fifoHandlerId;
int lifoHandlerId;
int ackHandlerId;

std::atomic<int> fifoRecvCount{0};
std::atomic<int> lifoRecvCount{0};
std::atomic<bool> ackSent{false};
std::atomic<int> ackCount{0};

struct XMsg {
  CmiMessageHeader header;
  int srcNode;
  int seq;
};

void sendAck() {
  void *ackMsg = CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(ackMsg, ackHandlerId);
  CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes, ackMsg);
}

void checkDone() {
  if (fifoRecvCount.load() >= numMsgs && lifoRecvCount.load() >= numMsgs) {
    bool expected = false;
    if (ackSent.compare_exchange_strong(expected, true)) {
      CmiPrintf("[node %d] received all %d cross-node NodeFifo and %d "
                "cross-node NodeLifo messages\n",
                CmiMyNode(), numMsgs, numMsgs);
      sendAck();
    }
  }
}

void fifoHandler(void *msg) {
  CmiFree(msg);
  fifoRecvCount.fetch_add(1);
  checkDone();
}

void lifoHandler(void *msg) {
  CmiFree(msg);
  lifoRecvCount.fetch_add(1);
  checkDone();
}

void ackHandler(void *msg) {
  CmiFree(msg);
  int count = ackCount.fetch_add(1) + 1;
  if (count == CmiNumNodes()) {
    CmiPrintf("All %d nodes confirmed cross-node NodeFifo/NodeLifo "
              "delivery via comm_backend::issueAm. Exiting.\n",
              CmiNumNodes());
    CmiExit(0);
  }
}

void sendTestMessages() {
  int destNode = (CmiMyNode() + 1) % CmiNumNodes();
  for (int i = 0; i < numMsgs; ++i) {
    XMsg *fmsg = (XMsg *)CmiAlloc(sizeof(XMsg));
    fmsg->header.messageSize = sizeof(XMsg);
    CmiSetHandler(fmsg, fifoHandlerId);
    fmsg->srcNode = CmiMyNode();
    fmsg->seq = i;
    CmiEnqueueNodeFifo(destNode, sizeof(XMsg), fmsg);

    XMsg *lmsg = (XMsg *)CmiAlloc(sizeof(XMsg));
    lmsg->header.messageSize = sizeof(XMsg);
    CmiSetHandler(lmsg, lifoHandlerId);
    lmsg->srcNode = CmiMyNode();
    lmsg->seq = i;
    CmiEnqueueNodeLifo(destNode, sizeof(XMsg), lmsg);
  }
}

CmiStartFn mymain(int argc, char **argv) {
  fifoHandlerId = CmiRegisterHandler((CmiHandler)fifoHandler);
  lifoHandlerId = CmiRegisterHandler((CmiHandler)lifoHandler);
  ackHandlerId = CmiRegisterHandler((CmiHandler)ackHandler);

  CmiGetArgInt(argv, "-num_msgs", &numMsgs);

  // Wait for all PEs of the node to finish handler registration/arg parsing
  CmiNodeAllBarrier();

  if (CmiNumNodes() < 2) {
    if (CmiMyPe() == 0) {
      CmiPrintf("cross_node_enqueue requires at least 2 nodes/processes to "
                "exercise the comm_backend::issueAm path; skipping.\n");
      CmiExit(0);
    }
    return 0;
  }

  // Only the first rank on each node drives the sends, to keep the expected
  // message count independent of the node's PE count.
  if (CmiMyRank() == 0) {
    sendTestMessages();
  }

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
