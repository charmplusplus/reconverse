#include "conv-topology.h"
#include <sys/socket.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <netinet/in.h> /* for sockaddr_in */
#include <ifaddrs.h> /* for getifaddrs */
#include <net/if.h> /* for IFF_RUNNING */
#include <cstring>


#include <algorithm>
#include <map>
#include <vector>

#define DEBUGP(x) /** CmiPrintf x; */

/** This scheme relies on using IP address to identify physical nodes
 * written by Gengbin Zheng  9/2008
 *
 * last updated 10/4/2009   Gengbin Zheng
 * added function CmiCpuTopologyEnabled() which retuens 1 when supported
 * when not supported return 0
 * all functions when cputopology not support, now act like a normal non-smp
 * case and all PEs are unique.
 *
 * major changes 10/28/09   Gengbin Zheng
 * - parameters changed from pe to node to be consistent with the function name
 * - two new functions:   CmiPhysicalNodeID and CmiPhysicalRank
 *
 * 3/5/2010   Gengbin Zheng
 * - use CmiReduce to optimize the collection of node info
 */

#if 1

#  include <stdio.h>
#  include <stdlib.h>
#  include <unistd.h>
#include <atomic>
#include <cerrno>


// int CmiNumCores(void)
// {
//   // PU count is the intended output here rather than literal cores
//   return CmiHwlocTopologyLocal.total_num_pus;
// }

static int affMsgsRecvd = 1;  // number of affinity messages received at PE0
#if defined(CPU_OR)
static cpu_set_t core_usage;  // used to record union of CPUs used by every PE in physical node
#endif
static int aff_is_set = 0;

static std::atomic<bool> cpuPhyAffCheckDone{};
static int cpuPhyNodeAffinityRecvHandlerIdx;

struct affMsg {
  char core[CmiMsgHeaderSizeBytes];
  #if defined(CPU_OR)
  cpu_set_t affinity;
  #endif
};

static void cpuPhyNodeAffinityRecvHandler(void *msg)
{
  static int count = 0;

  affMsg *m = (affMsg *)msg;
#if !defined(_WIN32) && defined(CPU_OR)
  CPU_OR(&core_usage, &core_usage, &m->affinity);
  affMsgsRecvd++;
#endif
  CmiFree(m);

  if (++count == CmiNumPesOnPhysicalNode(0) - 1)
    cpuPhyAffCheckDone = true;
}

void CmiInitCPUAffinity(char **argv) {
    CmiAssignOnce(&cpuPhyNodeAffinityRecvHandlerIdx, CmiRegisterHandler((CmiHandler)cpuPhyNodeAffinityRecvHandler));
}


static void cpuAffSyncWait(std::atomic<bool> & done)
{
  do
    CsdSchedulePoll();
  while (!done.load());

  CsdSchedulePoll();
}

void CmiInitMemAffinity(char **argv) {
    char *tmpstr = NULL;
    int maffinity_flag = CmiGetArgFlagDesc(argv,"+maffinity",
                                           "memory affinity");
    if (maffinity_flag && CmiMyPe()==0)
        CmiPrintf("memory affinity is not supported, +maffinity flag disabled.\n");

    /* consume the remaining possible arguments */
    CmiGetArgStringDesc(argv, "+memnodemap", &tmpstr, "define memory node mapping");
    CmiGetArgStringDesc(argv, "+mempol", &tmpstr, "define memory policy {bind, preferred or interleave} ");
}

#if defined(CPU_OR)
int get_thread_affinity(cpu_set_t *cpuset) {
  CPU_ZERO(cpuset);
  if ((errno = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), cpuset))) {
    perror("pthread_getaffinity");
    return -1;
  }
  return 0;
}
#endif


#if defined(CPU_OR)
int get_affinity(cpu_set_t *cpuset) {
  return get_thread_affinity(cpuset);
}
#endif

void CmiCheckAffinity(void)
{
  #if defined(CPU_OR)
  if (!CmiCpuTopologyEnabled()) return;  // only works if cpu topology enabled

  if (CmiNumPes() == 1)
    return;

  if (CmiMyPe() == 0)
  {
    // wait for every PE affinity from my physical node (for now only done on phy node 0)

    cpu_set_t my_aff;
    if (get_affinity(&my_aff) == -1) CmiAbort("get_affinity failed\n");
    CPU_OR(&core_usage, &core_usage, &my_aff); // add my affinity (pe0)

    cpuAffSyncWait(cpuPhyAffCheckDone);

  }
  else if (CmiPhysicalNodeID(CmiMyPe()) == 0)
  {
    // send my affinity to first PE on physical node (only done on phy node 0 for now)
    affMsg *m = (affMsg*)CmiAlloc(sizeof(affMsg));
    CmiSetHandler((char *)m, cpuPhyNodeAffinityRecvHandlerIdx);
    if (get_affinity(&m->affinity) == -1) { // put my affinity in msg
      CmiFree(m);
      CmiAbort("get_affinity failed\n");
    }
    CmiSyncSendAndFree(0, sizeof(affMsg), (void *)m);

    CsdSchedulePoll();
  }

  CmiBarrier();

  if (CmiMyPe() == 0)
  {
    // NOTE this test is simple and may not detect every possible case of
    // oversubscription
    const int N = CmiNumPesOnPhysicalNode(0);
    if (CPU_COUNT(&core_usage) < N) {
      // TODO suggest command line arguments?
      if (!aff_is_set) {
        CmiAbort("Multiple PEs assigned to same core. Set affinity "
        "options to correct or lower the number of threads, or pass +setcpuaffinity to ignore.\n");
      } else {
        CmiPrintf("WARNING: Multiple PEs assigned to same core, recommend "
        "adjusting processor affinity or passing +CmiSleepOnIdle to reduce "
        "interference.\n");
      }
    }
  }
  #endif
}

skt_ip_t _skt_invalid_ip={{0}};

skt_ip_t skt_my_ip(void)
{
  char hostname[1000];
  skt_ip_t ip = _skt_invalid_ip;
  int ifcount = 0;
    /* Code snippet from  Jens Alfke
 *     http://lists.apple.com/archives/macnetworkprog/2008/May/msg00013.html */
  struct ifaddrs *ifaces=0;
  if( getifaddrs(&ifaces) == 0 ) {
        struct ifaddrs *iface;
        for( iface=ifaces; iface; iface=iface->ifa_next ) {
            if( (iface->ifa_flags & IFF_UP) && ! (iface->ifa_flags & IFF_LOOPBACK) ) {
                const struct sockaddr_in *addr = (const struct sockaddr_in*)iface->ifa_addr;
                if( addr && addr->sin_family==AF_INET ) {
                    ifcount ++;
                    if ( ifcount==1 ) memcpy(&ip, &addr->sin_addr, sizeof(ip));
                }
            }
        }
        freeifaddrs(ifaces);
  }
  /* fprintf(stderr, "My IP is %d.%d.%d.%d\n", ip.data[0],ip.data[1],ip.data[2],ip.data[3]); */
  if (ifcount==1) return ip;

  return _skt_invalid_ip;
}

struct _procInfo
{
  skt_ip_t ip;
  int pe;
  int ncores;
  int rank;
  int nodeID;
};

typedef struct _hostnameMsg
{
  char core[CmiMsgHeaderSizeBytes];
  int n;
  _procInfo* procs;
} hostnameMsg;

typedef struct _nodeTopoMsg
{
  char core[CmiMsgHeaderSizeBytes];
  int* nodes;
} nodeTopoMsg;

// nodeIDs[pe] is the node number of processor pe
class CpuTopology
{
public:
  static int* nodeIDs;
  static int numPes;
  static int numNodes;
  static std::vector<int>* bynodes;
  static int supported;

  ~CpuTopology()
  {
    auto n = bynodes;
    bynodes = nullptr;
    delete[] n;
  }

  // return -1 when not supported
  int numUniqNodes()
  {
#  if 0
    if (numNodes != 0) return numNodes;
    int n = 0;
    for (int i=0; i<CmiNumPes(); i++) 
      if (nodeIDs[i] > n)
	n = nodeIDs[i];
    numNodes = n+1;
    return numNodes;
#  else
    if (numNodes > 0)
      return numNodes;  // already calculated
    std::vector<int> unodes(numPes);
    int i;
    for (i = 0; i < numPes; i++) unodes[i] = nodeIDs[i];
    std::sort(unodes.begin(), unodes.end());
    int last = -1;
    std::map<int, int> nodemap;  // nodeIDs can be out of range of [0,numNodes]
    for (i = 0; i < numPes; i++)
    {
      if (unodes[i] != last)
      {
        last = unodes[i];
        nodemap[unodes[i]] = numNodes;
        numNodes++;
      }
    }
    if (numNodes == 0)
    {
      numNodes = CmiNumNodes();
      numPes = CmiNumPes();
    }
    else
    {
      // re-number nodeIDs, which may be necessary e.g. on BlueGene/P
      for (i = 0; i < numPes; i++) nodeIDs[i] = nodemap[nodeIDs[i]];
      CpuTopology::supported = 1;
    }
    return numNodes;
#  endif
  }

  void sort()
  {
    int i;
    numUniqNodes();
    bynodes = new std::vector<int>[numNodes];
    if (supported)
    {
      for (i = 0; i < numPes; i++)
      {
        CmiAssert(nodeIDs[i] >= 0 &&
                  nodeIDs[i] <=
                      numNodes);  // Sanity check for bug that occurs on mpi-crayxt
        bynodes[nodeIDs[i]].push_back(i);
      }
    }
    else
    { /* not supported/enabled */
      for (i = 0; i < CmiNumPes(); i++) bynodes[CmiNodeOf(i)].push_back(i);
    }
  }

  void print()
  {
    int i;
    CmiPrintf("Charm++> Cpu topology info:\n");
    CmiPrintf("PE to node map: ");
    for (i = 0; i < CmiNumPes(); i++) CmiPrintf("%d ", nodeIDs[i]);
    CmiPrintf("\n");
    CmiPrintf("Node to PE map:\n");
    for (i = 0; i < numNodes; i++)
    {
      CmiPrintf("Chip #%d: ", i);
      for (int j = 0; j < bynodes[i].size(); j++) CmiPrintf("%d ", bynodes[i][j]);
      CmiPrintf("\n");
    }
  }
};

int* CpuTopology::nodeIDs = NULL;
int CpuTopology::numPes = 0;
int CpuTopology::numNodes = 0;
std::vector<int>* CpuTopology::bynodes = NULL;
int CpuTopology::supported = 0;

namespace CpuTopoDetails
{

static nodeTopoMsg* topomsg = NULL;
static std::map<skt_ip_t, _procInfo*> hostTable;

static int cpuTopoHandlerIdx;
static int cpuTopoRecvHandlerIdx;

static CpuTopology cpuTopo;
static int done = 0;
static int _noip = 0;

}  // namespace CpuTopoDetails

using namespace CpuTopoDetails;

// static void printTopology(int numNodes)
// {
//   // assume all nodes have same number of cores
//   const int ways = CmiHwlocTopologyLocal.num_pus;
//   if (ways > 1)
//     CmiPrintf(
//         "Charm++> Running on %d hosts (%d sockets x %d cores x %d PUs = %d-way SMP)\n",
//         numNodes, CmiHwlocTopologyLocal.num_sockets,
//         CmiHwlocTopologyLocal.num_cores / CmiHwlocTopologyLocal.num_sockets,
//         CmiHwlocTopologyLocal.num_pus / CmiHwlocTopologyLocal.num_cores, ways);
//   else
//     CmiPrintf("Charm++> Running on %d hosts\n", numNodes);
// }

static std::atomic<bool> cpuTopoSyncHandlerDone{};
#  if CMK_SMP && !CMK_SMP_NO_COMMTHD
extern void CommunicationServerThread(int sleepTime);
static std::atomic<bool> cpuTopoSyncCommThreadDone{};
#  endif

#  if CMK_SMP && !CMK_SMP_NO_COMMTHD
static void cpuTopoSyncWaitCommThread(std::atomic<bool>& done)
{
  do CommunicationServerThread(5);
  while (!done.load());

  CommunicationServerThread(5);
}
#  endif

static void cpuTopoSyncWait(std::atomic<bool>& done)
{
  do CsdSchedulePoll();
  while (!done.load());

  CsdSchedulePoll();
}

/* called on PE 0 */
static void cpuTopoHandler(void* m)
{
  _procInfo* rec;
  hostnameMsg* msg = (hostnameMsg*)m;
  int pe;

  if (topomsg == NULL)
  {
    int i;
    topomsg = (nodeTopoMsg*)CmiAlloc(sizeof(nodeTopoMsg) + CmiNumPes() * sizeof(int));
    CmiSetHandler((char*)topomsg, cpuTopoRecvHandlerIdx);
    topomsg->nodes = (int*)((char*)topomsg + sizeof(nodeTopoMsg));
    for (i = 0; i < CmiNumPes(); i++) topomsg->nodes[i] = -1;
  }
  CmiAssert(topomsg != NULL);

  msg->procs = (_procInfo*)((char*)msg + sizeof(hostnameMsg));
  CmiAssert(msg->n == CmiNumPes());
  for (int i = 0; i < msg->n; i++)
  {
    _procInfo* proc = msg->procs + i;

    /*   for debug
      skt_print_ip(str, msg->ip);
      printf("hostname: %d %s\n", msg->pe, str);
    */
    skt_ip_t& ip = proc->ip;
    pe = proc->pe;
    auto iter = hostTable.find(ip);
    if (iter != hostTable.end())
    {
      rec = iter->second;
    }
    else
    {
      proc->nodeID = pe;  // we will compact the node ID later
      rec = proc;
      hostTable.emplace(ip, proc);
    }
    topomsg->nodes[pe] = rec->nodeID;
    rec->rank++;
  }

  // printTopology(hostTable.size());

  hostTable.clear();
  CmiFree(msg);

  cpuTopoSyncHandlerDone = true;
}

/* called on each processor */
static void cpuTopoRecvHandler(void* msg)
{
  nodeTopoMsg* m = (nodeTopoMsg*)msg;
  m->nodes = (int*)((char*)m + sizeof(nodeTopoMsg));

  if (cpuTopo.nodeIDs == NULL)
  {
    cpuTopo.nodeIDs = m->nodes;
    cpuTopo.sort();
  }
  else
    CmiFree(m);
  done++;

  // if (CmiMyPe() == 0) cpuTopo.print();

  cpuTopoSyncHandlerDone = true;
}

// reduction function
static void* combineMessage(int* size, void* data, void** remote, int count)
{
  int i, j;
  int nprocs = ((hostnameMsg*)data)->n;
  if (count == 0)
    return data;
  for (i = 0; i < count; i++) nprocs += ((hostnameMsg*)remote[i])->n;
  *size = sizeof(hostnameMsg) + sizeof(_procInfo) * nprocs;
  hostnameMsg* msg = (hostnameMsg*)CmiAlloc(*size);
  msg->procs = (_procInfo*)((char*)msg + sizeof(hostnameMsg));
  msg->n = nprocs;
  CmiSetHandler((char*)msg, cpuTopoHandlerIdx);

  int n = 0;
  hostnameMsg* m = (hostnameMsg*)data;
  m->procs = (_procInfo*)((char*)m + sizeof(hostnameMsg));
  for (j = 0; j < m->n; j++) msg->procs[n++] = m->procs[j];
  for (i = 0; i < count; i++)
  {
    m = (hostnameMsg*)remote[i];
    m->procs = (_procInfo*)((char*)m + sizeof(hostnameMsg));
    for (j = 0; j < m->n; j++) msg->procs[n++] = m->procs[j];
  }
  return msg;
}

/******************  API implementation **********************/

int LrtsCpuTopoEnabled() { return CpuTopology::supported; }

int LrtsPeOnSameNode(int pe1, int pe2)
{
  int* nodeIDs = cpuTopo.nodeIDs;
  if (!cpuTopo.supported || nodeIDs == NULL)
    return CmiNodeOf(pe1) == CmiNodeOf(pe2);
  else
    return nodeIDs[pe1] == nodeIDs[pe2];
}

// return -1 when not supported
int LrtsNumNodes()
{
  if (!cpuTopo.supported)
    return CmiNumNodes();
  else
    return cpuTopo.numUniqNodes();
}

int LrtsNodeSize(int node)
{
  return !cpuTopo.supported ? CmiNodeSize(node) : (int)cpuTopo.bynodes[node].size();
}

// pelist points to system memory, user should not free it
void LrtsPeOnNode(int node, int** pelist, int* num)
{
  *num = cpuTopo.bynodes[node].size();
  if (pelist != NULL && *num > 0)
    *pelist = cpuTopo.bynodes[node].data();
}

int LrtsRankOf(int pe)
{
  if (!cpuTopo.supported)
    return CmiRankOf(pe);
  const std::vector<int>& v = cpuTopo.bynodes[cpuTopo.nodeIDs[pe]];
  int rank = 0;
  int npes = v.size();
  while (rank < npes && v[rank] < pe) rank++;  // already sorted
  CmiAssert(v[rank] == pe);
  return rank;
}

int LrtsNodeOf(int pe)
{
  if (!cpuTopo.supported)
    return CmiNodeOf(pe);
  return cpuTopo.nodeIDs[pe];
}

// the least number processor on the same physical node
int LrtsNodeFirst(int node)
{
  if (!cpuTopo.supported)
    return CmiNodeFirst(node);
  return cpuTopo.bynodes[node][0];
}

void LrtsInitCpuTopo(char** argv)
{
  static skt_ip_t myip;
  double startT;

  int obtain_flag = 1;  // default on
  int show_flag = 0;    // default not show topology

#  if __FAULT__
  obtain_flag = 0;
#  endif
  if (CmiGetArgFlagDesc(argv, "+obtain_cpu_topology", "obtain cpu topology info"))
    obtain_flag = 1;
  if (CmiGetArgFlagDesc(argv, "+skip_cpu_topology",
                        "skip the processof getting cpu topology info"))
    obtain_flag = 0;
  if (CmiGetArgFlagDesc(argv, "+show_cpu_topology", "Show cpu topology info"))
    show_flag = 1;

  CmiAssignOnce(&cpuTopoHandlerIdx, CmiRegisterHandler((CmiHandler)cpuTopoHandler));
  CmiAssignOnce(&cpuTopoRecvHandlerIdx,
                CmiRegisterHandler((CmiHandler)cpuTopoRecvHandler));

  if (!obtain_flag)
  {
    if (CmiMyRank() == 0)
      cpuTopo.sort();
    CmiNodeAllBarrier();
    CcdRaiseCondition(CcdTOPOLOGY_AVAIL);  // call callbacks
    return;
  }

  if (CmiMyPe() == 0)
  {
    startT = CmiWallTimer();
  }

#  if 0
  if (gethostname(hostname, 999)!=0) {
      strcpy(hostname, "");
  }
#  endif
#  if CMK_CRAYXE || CMK_CRAYXC || CMK_CRAYEX
  if (CmiMyRank() == 0)
  {
    int numPes = cpuTopo.numPes = CmiNumPes();
    int numNodes = CmiNumNodes();
    cpuTopo.nodeIDs = new int[numPes];
    CpuTopology::supported = 1;

    int nid;
    for (int i = 0; i < numPes; i++)
    {
      nid = getXTNodeID(CmiNodeOf(i), numNodes);
      cpuTopo.nodeIDs[i] = nid;
    }
    int prev = -1;
    nid = -1;

    // this assumes that all cores on a node have consecutive MPI rank IDs
    // and then changes nodeIDs to 0 to numNodes-1
    for (int i = 0; i < numPes; i++)
    {
      if (cpuTopo.nodeIDs[i] != prev)
      {
        prev = cpuTopo.nodeIDs[i];
        cpuTopo.nodeIDs[i] = ++nid;
      }
      else
        cpuTopo.nodeIDs[i] = nid;
    }
    cpuTopo.sort();
    if (CmiMyPe() == 0)
      printTopology(cpuTopo.numNodes);
  }
  CmiNodeAllBarrier();

#  else

  /* get my ip address */
  if (CmiMyRank() == 0)
  {
    myip = skt_my_ip(); /* not thread safe, so only calls on rank 0 */
    // fprintf(stderr, "[%d] IP is %d.%d.%d.%d\n", CmiMyPe(),
    // myip.data[0],myip.data[1],myip.data[2],myip.data[3]);
    cpuTopo.numPes = CmiNumPes();
  }

  CmiNodeAllBarrier();
  if (_noip)
  {
    if (CmiMyRank() == 0)
      cpuTopo.sort();
    CcdRaiseCondition(CcdTOPOLOGY_AVAIL);  // call callbacks
    return;
  }

#    if CMK_SMP && !CMK_SMP_NO_COMMTHD
  if (CmiInCommThread())
  {
    cpuTopoSyncWaitCommThread(cpuTopoSyncCommThreadDone);
  }
  else
#    endif
  {
    /* prepare a msg to send */
    hostnameMsg* msg = (hostnameMsg*)CmiAlloc(sizeof(hostnameMsg) + sizeof(_procInfo));
    msg->n = 1;
    msg->procs = (_procInfo*)((char*)msg + sizeof(hostnameMsg));
    CmiSetHandler((char*)msg, cpuTopoHandlerIdx);
    auto proc = &msg->procs[0];
    proc->pe = CmiMyPe();
    proc->ip = myip;
    // NCORES IS NOT BEING DEFINED
    proc->ncores = 0;
    proc->rank = 0;
    proc->nodeID = 0;
    CmiReduce(msg, sizeof(hostnameMsg) + sizeof(_procInfo), combineMessage);

    cpuTopoSyncWait(cpuTopoSyncHandlerDone);

    if (CmiMyRank() == 0)
    {
      if (CmiMyPe() == 0)
      {
        CmiSyncNodeBroadcastAllAndFree(sizeof(nodeTopoMsg) + CmiNumPes() * sizeof(int),
                                       (char*)topomsg);

        CsdSchedulePoll();
      }

#    if CMK_SMP && !CMK_SMP_NO_COMMTHD
      cpuTopoSyncCommThreadDone = true;
#    endif
    }
  }

  CmiBarrier();

  if (CmiMyPe() == 0)
  {
    CmiPrintf("Charm++> cpu topology info is gathered in %.3f seconds.\n",
              CmiWallTimer() - startT);
  }
#  endif

  // now every one should have the node info
  CcdRaiseCondition(CcdTOPOLOGY_AVAIL);  // call callbacks
  if (CmiMyPe() == 0 && show_flag)
    cpuTopo.print();
}

#else /* not supporting cpu topology */

extern "C" void LrtsInitCpuTopo(char** argv)
{
  /* do nothing */
  int obtain_flag =
      CmiGetArgFlagDesc(argv, "+obtain_cpu_topology", "obtain cpu topology info");
  CmiGetArgFlagDesc(argv, "+skip_cpu_topology",
                    "skip the processof getting cpu topology info");
  CmiGetArgFlagDesc(argv, "+show_cpu_topology", "Show cpu topology info");
}

#endif

int CmiCpuTopologyEnabled() { return LrtsCpuTopoEnabled(); }
int CmiPeOnSamePhysicalNode(int pe1, int pe2) { return LrtsPeOnSameNode(pe1, pe2); }
int CmiNumPhysicalNodes() { return LrtsNumNodes(); }
int CmiNumPesOnPhysicalNode(int node) { return LrtsNodeSize(node); }
void CmiGetPesOnPhysicalNode(int node, int** pelist, int* num)
{
  LrtsPeOnNode(node, pelist, num);
}
int CmiPhysicalRank(int pe) { return LrtsRankOf(pe); }
//int CmiPhysicalNodeID(int pe) { return LrtsNodeOf(pe); }
int CmiGetFirstPeOnPhysicalNode(int node) { return LrtsNodeFirst(node); }
void CmiInitCPUTopology(char** argv) { LrtsInitCpuTopo(argv); }
