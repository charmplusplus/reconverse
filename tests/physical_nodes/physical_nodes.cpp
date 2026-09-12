// Physical-node topology queries. On a single host every PE shares one
// physical node, so the queries have exact expected answers; on several
// hosts the consistency checks still hold.
#include "converse.h"
#include <cstdlib>

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("physical_nodes: %s", what);
}

static void mymain(int argc, char **argv) {
  CmiBarrier();
  if (CmiMyPe() == 0) {
    int npes = CmiNumPes(), nnodes = CmiNumNodes();
    int nphys = CmiNumPhysicalNodes();
    CmiPrintf("CmiNumPhysicalNodes() = %d\n", nphys);
    CmiPrintf("CmiNumNodes()         = %d\n", nnodes);
    CmiPrintf("CmiNumPes()           = %d\n", npes);
    CmiPrintf("CmiNumCores()         = %d\n", CmiNumCores());
    CmiPrintf("CmiCpuTopologyEnabled() = %d\n", CmiCpuTopologyEnabled());
    for (int pe = 0; pe < npes; pe++)
      CmiPrintf("PE %d: node=%d physicalNode=%d physicalRank=%d\n", pe,
                CmiNodeOf(pe), CmiPhysicalNodeID(pe), CmiPhysicalRank(pe));

    check(nphys >= 1 && nphys <= nnodes, "physical node count out of range");
    check(CmiNumCores() >= 1, "no cores");

    // Every PE belongs to exactly one physical node; the per-node PE lists
    // partition the PEs, and each list's first PE agrees with
    // CmiGetFirstPeOnPhysicalNode.
    int total = 0;
    for (int pn = 0; pn < nphys; pn++) {
      int *pelist = NULL, num = 0;
      CmiGetPesOnPhysicalNode(pn, &pelist, &num);
      check(num == CmiNumPesOnPhysicalNode(pn), "list length vs count");
      check(num >= 1, "physical node with no PEs");
      check(pelist[0] == CmiGetFirstPeOnPhysicalNode(pn), "first PE");
      for (int i = 0; i < num; i++) {
        check(CmiPhysicalNodeID(pelist[i]) == pn, "PE listed under wrong node");
        check(CmiPeOnSamePhysicalNode(pelist[0], pelist[i]),
              "PEs of one node not reported on the same node");
      }
      total += num;
    }
    check(total == npes, "physical node lists do not cover all PEs");

    // Rank of a logical node among the nodes of its physical node: distinct
    // and below the number of logical nodes on that physical node.
    for (int pn = 0; pn < nphys; pn++) {
      int *pelist = NULL, num = 0;
      CmiGetPesOnPhysicalNode(pn, &pelist, &num);
      int seen[1024] = {0}, nodesHere = 0;
      for (int i = 0; i < num; i++)
        if (CmiPhysicalRank(pelist[i]) == 0 ||
            CmiNodeFirst(CmiNodeOf(pelist[i])) == pelist[i])
          nodesHere++;
      for (int i = 0; i < num; i++) {
        int node = CmiNodeOf(pelist[i]);
        if (CmiNodeFirst(node) != pelist[i])
          continue;
        int r = CmiNodeRankOnPhysicalNode(node);
        CmiPrintf("node %d: rank %d of %d on physical node %d\n", node, r,
                  nodesHere, pn);
        check(r >= 0 && r < nodesHere && r < 1024, "node rank out of range");
        check(!seen[r], "two nodes share a rank on one physical node");
        seen[r] = 1;
      }
    }
    if (nphys == 1)
      check(CmiPeOnSamePhysicalNode(0, npes - 1), "single host, PEs apart");
    CmiPrintf("physical node topology consistent\n");
  }
  CmiBarrier();
  if (CmiMyPe() == 0)
    CmiExit(0);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
