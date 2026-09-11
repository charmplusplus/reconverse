/* Exercises Charm++ partitions. Three things have to hold at once: each
 * partition sees a run size of its own while the global one stays reachable
 * through the *Global accessors; an ordinary send stays inside the sender's
 * partition even when that partition spans several processes, which is only
 * true if the partition-local node number is translated back to the process
 * the communication backend addresses; and the CmiInter* calls land on exactly
 * the intended peer in another partition, translated once and not twice. The
 * PE-level and node-level send paths translate separately, so both are
 * covered. Running with more processes than partitions is what makes the test
 * meaningful, since a partition then spans more than one process. */

#include <atomic>
#include <converse.h>
#include <stdlib.h>

typedef struct {
  char core[CmiMsgHeaderSizeBytes];
  int srcPartition;
  int srcPe;
  int srcNode;
  int tag;
} TestMsg;

enum { TAG_PE_INTRA, TAG_PE_INTER, TAG_NODE_INTRA, TAG_NODE_INTER };

CpvStaticDeclare(int, peHIdx);
CpvStaticDeclare(int, nodeHIdx);
CpvStaticDeclare(int, nodeDoneHIdx);
CpvStaticDeclare(int, intraSeen);
CpvStaticDeclare(int, interSeen);
CpvStaticDeclare(int, nodeDone);
CpvStaticDeclare(char *, intraFrom);

static std::atomic<int> nodeSeen{0};

static int prevPartition(void) {
  return (CmiMyPartition() + CmiNumPartitions() - 1) % CmiNumPartitions();
}

static int nextPartition(void) {
  return (CmiMyPartition() + 1) % CmiNumPartitions();
}

static int nodeExpected(void) {
  return CmiNumNodes() + (CmiNumPartitions() > 1 ? 1 : 0);
}

static TestMsg *newMsg(int tag, int handler) {
  TestMsg *msg = (TestMsg *)CmiAlloc(sizeof(TestMsg));
  msg->srcPartition = CmiMyPartition();
  msg->srcPe = CmiMyPe();
  msg->srcNode = CmiMyNode();
  msg->tag = tag;
  CmiSetHandler(msg, handler);
  return msg;
}

static void maybeDone(void) {
  if (CpvAccess(intraSeen) < CmiNumPes())
    return;
  if (CmiNumPartitions() > 1 && CpvAccess(interSeen) < 1)
    return;
  if (!CpvAccess(nodeDone))
    return;
  CsdExitScheduler();
}

static void peHandler(void *env) {
  TestMsg *msg = (TestMsg *)env;

  if (msg->tag == TAG_PE_INTRA) {
    if (msg->srcPartition != CmiMyPartition())
      CmiAbort("[part %d pe %d] an intra-partition message arrived from "
               "partition %d, so it left the partition\n",
               CmiMyPartition(), CmiMyPe(), msg->srcPartition);
    if (msg->srcPe < 0 || msg->srcPe >= CmiNumPes())
      CmiAbort("[part %d pe %d] an intra-partition message claims source PE "
               "%d, outside this partition's 0..%d\n",
               CmiMyPartition(), CmiMyPe(), msg->srcPe, CmiNumPes() - 1);
    if (CpvAccess(intraFrom)[msg->srcPe])
      CmiAbort("[part %d pe %d] a second intra-partition message arrived from "
               "PE %d\n",
               CmiMyPartition(), CmiMyPe(), msg->srcPe);
    CpvAccess(intraFrom)[msg->srcPe] = 1;
    CpvAccess(intraSeen)++;
  } else if (msg->tag == TAG_PE_INTER) {
    if (msg->srcPartition != prevPartition())
      CmiAbort("[part %d pe %d] an inter-partition message arrived from "
               "partition %d, expected %d\n",
               CmiMyPartition(), CmiMyPe(), msg->srcPartition, prevPartition());
    if (msg->srcPe != CmiMyPe())
      CmiAbort("[part %d pe %d] an inter-partition message meant for PE %d "
               "arrived here\n",
               CmiMyPartition(), CmiMyPe(), msg->srcPe);
    if (++CpvAccess(interSeen) > 1)
      CmiAbort("[part %d pe %d] %d inter-partition messages arrived, "
               "expected 1\n",
               CmiMyPartition(), CmiMyPe(), CpvAccess(interSeen));
  } else {
    CmiAbort("[part %d pe %d] tag %d arrived on the PE queue\n",
             CmiMyPartition(), CmiMyPe(), msg->tag);
  }

  CmiFree(msg);
  maybeDone();
}

static void nodeHandler(void *env) {
  TestMsg *msg = (TestMsg *)env;

  if (msg->tag == TAG_NODE_INTRA) {
    if (msg->srcPartition != CmiMyPartition())
      CmiAbort("[part %d node %d] an intra-partition node message arrived from "
               "partition %d\n",
               CmiMyPartition(), CmiMyNode(), msg->srcPartition);
    if (msg->srcNode < 0 || msg->srcNode >= CmiNumNodes())
      CmiAbort("[part %d node %d] an intra-partition node message claims "
               "source process %d, outside this partition's 0..%d\n",
               CmiMyPartition(), CmiMyNode(), msg->srcNode, CmiNumNodes() - 1);
  } else if (msg->tag == TAG_NODE_INTER) {
    if (msg->srcPartition != prevPartition())
      CmiAbort("[part %d node %d] an inter-partition node message arrived from "
               "partition %d, expected %d\n",
               CmiMyPartition(), CmiMyNode(), msg->srcPartition,
               prevPartition());
    if (msg->srcNode != CmiMyNode())
      CmiAbort("[part %d node %d] an inter-partition node message meant for "
               "process %d arrived here\n",
               CmiMyPartition(), CmiMyNode(), msg->srcNode);
  } else {
    CmiAbort("[part %d node %d] tag %d arrived on the node queue\n",
             CmiMyPartition(), CmiMyNode(), msg->tag);
  }
  CmiFree(msg);

  int seen = nodeSeen.fetch_add(1) + 1;
  if (seen > nodeExpected())
    CmiAbort("[part %d node %d] %d node messages arrived, expected %d\n",
             CmiMyPartition(), CmiMyNode(), seen, nodeExpected());

  /* Any PE on this process can drain the node queue, so the one that takes the
     last message has to wake the others rather than let them wait on a counter
     no further message will touch. */
  if (seen == nodeExpected()) {
    TestMsg *done = newMsg(TAG_PE_INTRA, CpvAccess(nodeDoneHIdx));
    CmiWithinNodeBroadcast(sizeof(TestMsg), done);
    CmiFree(done);
  }
}

static void nodeDoneHandler(void *env) {
  CmiFree(env);
  CpvAccess(nodeDone) = 1;
  maybeDone();
}

static void checkIdentity(int expectPartitions) {
  int np = CmiNumPartitions();
  int mp = CmiMyPartition();

  if (np < 1)
    CmiAbort("CmiNumPartitions() is %d\n", np);
  if (expectPartitions > 0 && np != expectPartitions)
    CmiAbort("%d partitions were requested but the run has %d, so the option "
             "was ignored\n",
             expectPartitions, np);
  if (mp < 0 || mp >= np)
    CmiAbort("CmiMyPartition() is %d, outside 0..%d\n", mp, np - 1);

  if (CmiNumNodesGlobal() != np * CmiNumNodes())
    CmiAbort("[part %d] the job has %d processes but this partition reports "
             "%d of %d partitions\n",
             mp, CmiNumNodesGlobal(), CmiNumNodes(), np);
  if (CmiNumPesGlobal() != np * CmiNumPes())
    CmiAbort("[part %d] the job has %d PEs but this partition reports %d of "
             "%d partitions\n",
             mp, CmiNumPesGlobal(), CmiNumPes(), np);

  if (CmiMyNodeGlobal() != mp * CmiNumNodes() + CmiMyNode())
    CmiAbort("[part %d] CmiMyNodeGlobal() is %d, expected %d for local "
             "process %d\n",
             mp, CmiMyNodeGlobal(), mp * CmiNumNodes() + CmiMyNode(),
             CmiMyNode());
  if (CmiMyPeGlobal() != mp * CmiNumPes() + CmiMyPe())
    CmiAbort("[part %d] CmiMyPeGlobal() is %d, expected %d for local PE %d\n",
             mp, CmiMyPeGlobal(), mp * CmiNumPes() + CmiMyPe(), CmiMyPe());

  if (CmiGetNodeGlobal(CmiMyNode(), mp) != CmiMyNodeGlobal())
    CmiAbort("[part %d] CmiGetNodeGlobal(%d, %d) is %d, expected %d\n", mp,
             CmiMyNode(), mp, CmiGetNodeGlobal(CmiMyNode(), mp),
             CmiMyNodeGlobal());
  if (CmiGetPeGlobal(CmiMyPe(), mp) != CmiMyPeGlobal())
    CmiAbort("[part %d] CmiGetPeGlobal(%d, %d) is %d, expected %d\n", mp,
             CmiMyPe(), mp, CmiGetPeGlobal(CmiMyPe(), mp), CmiMyPeGlobal());

  for (int p = 0; p < np; p++) {
    if (CmiPartitionSize(p) != CmiNumNodes())
      CmiAbort("[part %d] partition %d holds %d processes, expected %d\n", mp,
               p, CmiPartitionSize(p), CmiNumNodes());
    if (CmiGetNodeGlobal(0, p) != p * CmiNumNodes())
      CmiAbort("[part %d] process 0 of partition %d maps to global process "
               "%d, expected %d\n",
               mp, p, CmiGetNodeGlobal(0, p), p * CmiNumNodes());
  }
}

CmiStartFn mymain(int argc, char **argv) {
  CpvInitialize(int, peHIdx);
  CpvInitialize(int, nodeHIdx);
  CpvInitialize(int, nodeDoneHIdx);
  CpvInitialize(int, intraSeen);
  CpvInitialize(int, interSeen);
  CpvInitialize(int, nodeDone);
  CpvInitialize(char *, intraFrom);

  CpvAccess(peHIdx) = CmiRegisterHandler((CmiHandler)peHandler);
  CpvAccess(nodeHIdx) = CmiRegisterHandler((CmiHandler)nodeHandler);
  CpvAccess(nodeDoneHIdx) = CmiRegisterHandler((CmiHandler)nodeDoneHandler);
  CpvAccess(intraSeen) = 0;
  CpvAccess(interSeen) = 0;
  CpvAccess(nodeDone) = 0;
  CpvAccess(intraFrom) = (char *)calloc(CmiNumPes(), 1);

  int expectPartitions = 0;
  CmiGetArgInt(argv, "-expect-partitions", &expectPartitions);
  checkIdentity(expectPartitions);

  if (CmiMyPeGlobal() == 0)
    CmiPrintf("Partition test: %d partitions of %d processes and %d PEs each, "
              "%d processes and %d PEs in total\n",
              CmiNumPartitions(), CmiNumNodes(), CmiNumPes(),
              CmiNumNodesGlobal(), CmiNumPesGlobal());

  CmiNodeBarrier();

  for (int pe = 0; pe < CmiNumPes(); pe++)
    CmiSyncSendAndFree(pe, sizeof(TestMsg),
                       newMsg(TAG_PE_INTRA, CpvAccess(peHIdx)));

  if (CmiNumPartitions() > 1)
    CmiInterSyncSendAndFree(CmiMyPe(), nextPartition(), sizeof(TestMsg),
                            newMsg(TAG_PE_INTER, CpvAccess(peHIdx)));

  if (CmiMyRank() == 0) {
    for (int node = 0; node < CmiNumNodes(); node++)
      CmiSyncNodeSendAndFree(node, sizeof(TestMsg),
                             newMsg(TAG_NODE_INTRA, CpvAccess(nodeHIdx)));

    if (CmiNumPartitions() > 1)
      CmiInterSyncNodeSendAndFree(CmiMyNode(), nextPartition(),
                                  sizeof(TestMsg),
                                  newMsg(TAG_NODE_INTER, CpvAccess(nodeHIdx)));
  }

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
