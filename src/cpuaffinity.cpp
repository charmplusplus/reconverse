/*
 This scheme relies on using IP address to identify hosts and assign
 cpu affinity.

 When CMK_NO_SOCKETS, which is typically on cray xt3 and bluegene/L,
 there is no hostname.
 *
 * last updated 3/20/2010   Gengbin Zheng
 * new options +pemap +commmap takes complex pattern of a list of cores
*/

#include "converse_internal.h"

#ifdef RECONVERSE_ENABLE_CPU_AFFINITY
#include "hwloc.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

static int affMsgsRecvd = 1;  // number of affinity messages received at PE0
#if defined(CPU_OR)
static cpu_set_t core_usage;  // used to record union of CPUs used by every PE in physical node
#endif
static int aff_is_set = 0;

static std::atomic<bool> cpuPhyAffCheckDone{};

struct affMsg {
  char core[CmiMsgHeaderSizeBytes];
  #if defined(CPU_OR)
  cpu_set_t affinity;
  #endif
};

CmiHwlocTopology CmiHwlocTopologyLocal;
static int cpuPhyNodeAffinityRecvHandlerIdx;

// topology is the set of resources available to this process
// legacy_topology includes resources disallowed by the system, to implement
// CmiNumCores
static hwloc_topology_t topology, legacy_topology;

int CmiNumCores(void)
{
  // PU count is the intended output here rather than literal cores
  return CmiHwlocTopologyLocal.total_num_pus;
}

static int search_pemap(char *pecoremap, int pe)
{
  int *map = (int *)malloc(CmiNumPesGlobal()*sizeof(int));
  char *ptr = NULL;
  int h, i, j, k, count;
  int plusarr[128];
  char *str;

  char *mapstr = (char*)malloc(strlen(pecoremap)+1);
  strcpy(mapstr, pecoremap);

  str = strtok_r(mapstr, ",", &ptr);
  count = 0;
  while (str && count < CmiNumPesGlobal())
  {
      int hasdash=0, hascolon=0, hasdot=0, hasstar1=0, hasstar2=0, numplus=0;
      int start, end, stride=1, block=1;
      int iter=1;
      plusarr[0] = 0;
      for (i=0; i<strlen(str); i++) {
          if (str[i] == '-' && i!=0) hasdash=1;
          else if (str[i] == ':') hascolon=1;
          else if (str[i] == '.') hasdot=1;
          else if (str[i] == 'x') hasstar1=1;
          else if (str[i] == 'X') hasstar2=1;
          else if (str[i] == '+') {
            if (str[i+1] == '+' || str[i+1] == '-') {
              printf("Warning: Check the format of \"%s\".\n", str);
            } else if (sscanf(&str[i], "+%d", &plusarr[++numplus]) != 1) {
              printf("Warning: Check the format of \"%s\".\n", str);
              --numplus;
            }
          }
      }
      if (hasstar1 || hasstar2) {
          if (hasstar1) sscanf(str, "%dx", &iter);
          if (hasstar2) sscanf(str, "%dX", &iter);
          while (*str!='x' && *str!='X') str++;
          str++;
      }
      if (hasdash) {
          if (hascolon) {
            if (hasdot) {
              if (sscanf(str, "%d-%d:%d.%d", &start, &end, &stride, &block) != 4)
                 printf("Warning: Check the format of \"%s\".\n", str);
            }
            else {
              if (sscanf(str, "%d-%d:%d", &start, &end, &stride) != 3)
                 printf("Warning: Check the format of \"%s\".\n", str);
            }
          }
          else {
            if (sscanf(str, "%d-%d", &start, &end) != 2)
                 printf("Warning: Check the format of \"%s\".\n", str);
          }
      }
      else {
          sscanf(str, "%d", &start);
          end = start;
      }
      if (block > stride) {
        printf("Warning: invalid block size in \"%s\" ignored.\n", str);
        block=1;
      }
      //if (CmiMyPe() == 0) printf("iter: %d start: %d end: %d stride: %d, block: %d. plus %d \n", iter, start, end, stride, block, numplus);
      for (k = 0; k<iter; k++) {
        for (i = start; i<=end; i+=stride) {
          for (j=0; j<block; j++) {
            if (i+j>end) break;
            for (h=0; h<=numplus; h++) {
              map[count++] = i+j+plusarr[h];
              if (count == CmiNumPesGlobal()) break;
            }
            if (count == CmiNumPesGlobal()) break;
          }
          if (count == CmiNumPesGlobal()) break;
        }
        if (count == CmiNumPesGlobal()) break;
      }
      str = strtok_r(NULL, ",", &ptr);
  }
  i = map[pe % count];

  free(map);
  free(mapstr);
  return i;
}

// Check if provided mapping string uses logical indices as assigned by hwloc.
// Logical indices are used if the first character of the map string is an L
// (case-insensitive).
static int check_logical_indices(char **mapptr) {
  if ((*mapptr)[0] == 'l' || (*mapptr)[0] == 'L') {
    (*mapptr)++; // Exclude the L character from the string
    return 1;
  }

  return 0;
}

/*print_thread_affinity from old converse*/
int CmiPrintCPUAffinity(void) {
  pthread_t thread = pthread_self();

  hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
  // And try to bind ourself there. */
  if (hwloc_get_cpubind(topology, cpuset, HWLOC_CPUBIND_THREAD) == -1) {
    int error = errno;
    CmiPrintf("[%d] thread CPU affinity mask is unknown %s\n", CmiMyPe(), strerror(error));
    hwloc_bitmap_free(cpuset);
    return -1;
  }

  char *str;
  hwloc_bitmap_asprintf(&str, cpuset);
  CmiPrintf("[%d] thread CPU affinity mask is %s\n", CmiMyPe(), str);
  free(str);
  hwloc_bitmap_free(cpuset);
  return 0;

}

static void cpuAffSyncWait(std::atomic<bool> & done)
{
  do
    CsdSchedulePoll();
  while (!done.load());

  CsdSchedulePoll();
}

static void cpuPhyNodeAffinityRecvHandler(void *msg)
{
  static int count = 0;

  affMsg *m = (affMsg *)msg;
#if defined(CPU_OR)
  CPU_OR(&core_usage, &core_usage, &m->affinity);
  affMsgsRecvd++;
#endif
  CmiFree(m);

  if (++count == CmiNumPesOnPhysicalNode(0) - 1)
    cpuPhyAffCheckDone = true;
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

void CmiInitHwlocTopology(void) {
  int depth;

  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);

  // packages == sockets
  depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PACKAGE);
  CmiHwlocTopologyLocal.num_sockets =
      depth != HWLOC_TYPE_DEPTH_UNKNOWN
          ? hwloc_get_nbobjs_by_depth(topology, depth)
          : 1;

  // cores
  depth = hwloc_get_type_depth(topology, HWLOC_OBJ_CORE);
  CmiHwlocTopologyLocal.num_cores =
      depth != HWLOC_TYPE_DEPTH_UNKNOWN
          ? hwloc_get_nbobjs_by_depth(topology, depth)
          : 1;

  // PUs
  depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PU);
  CmiHwlocTopologyLocal.num_pus =
      depth != HWLOC_TYPE_DEPTH_UNKNOWN
          ? hwloc_get_nbobjs_by_depth(topology, depth)
          : 1;

  // Legacy: Determine the system's total PU count

  hwloc_topology_init(&legacy_topology);
#if HWLOC_API_VERSION >= 0x00020000
  // HWLOC 2.0+ supports HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED
  hwloc_topology_set_flags(legacy_topology,
                           hwloc_topology_get_flags(legacy_topology) |
                               HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
#else
  // For HWLOC 1.x, use HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM to include all PUs
  hwloc_topology_set_flags(legacy_topology,
                           hwloc_topology_get_flags(legacy_topology) |
                               HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM);
#endif
  hwloc_topology_load(legacy_topology);

  depth = hwloc_get_type_depth(legacy_topology, HWLOC_OBJ_PU);
  CmiHwlocTopologyLocal.total_num_pus =
      depth != HWLOC_TYPE_DEPTH_UNKNOWN
          ? hwloc_get_nbobjs_by_depth(legacy_topology, depth)
          : 1;
}

static int set_process_affinity(hwloc_cpuset_t cpuset)
{
  pid_t process = getpid();
  #define PRINTF_PROCESS "%d"
  if (hwloc_set_proc_cpubind(topology, process, cpuset, HWLOC_CPUBIND_PROCESS|HWLOC_CPUBIND_STRICT))
  {
    char *str;
    int error = errno;
    hwloc_bitmap_asprintf(&str, cpuset);
    CmiPrintf("HWLOC> Couldn't bind to cpuset %s: %s\n", str, strerror(error));
    free(str);
    return -1;
  }
  return 0;
  #undef PRINTF_PROCESS
}


static int set_thread_affinity(hwloc_cpuset_t cpuset)
{
  pthread_t thread = pthread_self();
  if (hwloc_set_thread_cpubind(topology, thread, cpuset, HWLOC_CPUBIND_THREAD|HWLOC_CPUBIND_STRICT))
  {
    char *str;
    int error = errno;
    hwloc_bitmap_asprintf(&str, cpuset);
    CmiPrintf("HWLOC> Couldn't bind to cpuset %s: %s\n", str, strerror(error));
    free(str);
    return -1;
  }
  return 0;
}

static void bind_process_only(hwloc_obj_type_t process_unit)
{
  hwloc_cpuset_t cpuset;

  int process_unitcount = hwloc_get_nbobjs_by_type(topology, process_unit);

  int process_assignment = CmiMyRank() % process_unitcount;

  hwloc_obj_t process_obj = hwloc_get_obj_by_type(topology, process_unit, process_assignment);
  set_process_affinity(process_obj->cpuset);
}

static void bind_threads_only(hwloc_obj_type_t thread_unit)
{
  hwloc_cpuset_t cpuset;

  int thread_unitcount = hwloc_get_nbobjs_by_type(topology, thread_unit);

  int thread_assignment = CmiMyRank() % thread_unitcount;

  hwloc_obj_t thread_obj = hwloc_get_obj_by_type(topology, thread_unit, thread_assignment);
  hwloc_cpuset_t thread_cpuset = hwloc_bitmap_dup(thread_obj->cpuset);
  hwloc_bitmap_singlify(thread_cpuset);
  set_thread_affinity(thread_cpuset);
  hwloc_bitmap_free(thread_cpuset);
}

static void bind_process_and_threads(hwloc_obj_type_t process_unit, hwloc_obj_type_t thread_unit)
{
  hwloc_cpuset_t cpuset;

  int process_unitcount = hwloc_get_nbobjs_by_type(topology, process_unit);

  int process_assignment = CmiMyRank() % process_unitcount;

  hwloc_obj_t process_obj = hwloc_get_obj_by_type(topology, process_unit, process_assignment);
  set_process_affinity(process_obj->cpuset);

  int thread_unitcount = hwloc_get_nbobjs_inside_cpuset_by_type(topology, process_obj->cpuset, thread_unit);

  int thread_assignment = CmiMyRank() % thread_unitcount;

  hwloc_obj_t thread_obj = hwloc_get_obj_inside_cpuset_by_type(topology, process_obj->cpuset, thread_unit, thread_assignment);
  hwloc_cpuset_t thread_cpuset = hwloc_bitmap_dup(thread_obj->cpuset);
  hwloc_bitmap_singlify(thread_cpuset);
  set_thread_affinity(thread_cpuset);
  hwloc_bitmap_free(thread_cpuset);
}

static int set_default_affinity(void){
  char *s;
  int n = -1;

  if ((s = getenv("CmiProcessPerSocket")))
  {
    n = atoi(s);
    if (getenv("CmiOneWthPerCore"))
      bind_process_and_threads(HWLOC_OBJ_PACKAGE, HWLOC_OBJ_CORE);
    else if (getenv("CmiOneWthPerPU"))
      bind_process_and_threads(HWLOC_OBJ_PACKAGE, HWLOC_OBJ_PU);
    else
      bind_process_only(HWLOC_OBJ_PACKAGE);
  }
  else if ((s = getenv("CmiProcessPerCore")))
  {
    n = atoi(s);
    if (getenv("CmiOneWthPerPU"))
      bind_process_and_threads(HWLOC_OBJ_CORE, HWLOC_OBJ_PU);
    else
      bind_process_only(HWLOC_OBJ_CORE);
  }
  else if ((s = getenv("CmiProcessPerPU")))
  {
    n = atoi(s);
    bind_process_only(HWLOC_OBJ_PU);
  }
  else // if ((s = getenv("CmiProcessPerHost")))
  {
    if (getenv("CmiOneWthPerSocket"))
    {
      n = 0;
      bind_threads_only(HWLOC_OBJ_PACKAGE);
    }
    else if (getenv("CmiOneWthPerCore"))
    {
      n = 0;
      bind_threads_only(HWLOC_OBJ_CORE);
    }
    else if (getenv("CmiOneWthPerPU"))
    {
      n = 0;
      bind_threads_only(HWLOC_OBJ_PU);
    }
  }

  return n != -1;
}

void CmiInitCPUAffinity(char **argv) {
    #if defined(CPU_OR)
    // check for flags
    int affinity_flag = CmiGetArgFlagDesc(argv,"+setcpuaffinity", "set cpu affinity");
    int pemap_logical_flag = 0;
    int show_affinity_flag = 0;
    char *pemap = NULL;
    // 0 if OS-assigned, 1 if logical hwloc assigned
    // for now, stick with os-assigned only
    // also no commap, we have no commthreads
    CmiGetArgStringDesc(argv, "+pemap", &pemap, "define pe to core mapping");
    if (pemap!=NULL) affinity_flag = 1;
    if (pemap != NULL) pemap_logical_flag = check_logical_indices(&pemap);
    show_affinity_flag = CmiGetArgFlagDesc(argv,"+showcpuaffinity", "print cpu affinity");
    CmiAssignOnce(&cpuPhyNodeAffinityRecvHandlerIdx, CmiRegisterHandler((CmiHandler)cpuPhyNodeAffinityRecvHandler));
    // setting default affinity (always needed, not the same as setting cpu affinity)
    int done = 0;
    CmiNodeAllBarrier();
    /* must bind the rank 0 which is the main thread first */
    /* binding the main thread seems to change binding for all threads */
    if (CmiMyRank() == 0) {
        done = set_default_affinity();
    }
    CmiNodeAllBarrier();
    if (CmiMyRank() != 0) {
      done = set_default_affinity();
    }
    if (done) {
      if (show_affinity_flag) CmiPrintCPUAffinity();
      return;
    }
    if (CmiMyRank() ==0) {
     aff_is_set = affinity_flag;
     CPU_ZERO(&core_usage);
  }
    //set cmi affinity
    if (!affinity_flag) {
      if (show_affinity_flag && CmiMyPe() == 0) CmiPrintCPUAffinity();
      if (CmiMyPe() == 0) CmiPrintf("Charm++> cpu affinity NOT enabled.\n");
      return;
    }
    if (CmiMyPe() == 0) {
     CmiPrintf("Charm++> cpu affinity enabled. \n");
     if (pemap!=NULL)
       CmiPrintf("Charm++> cpuaffinity PE-core map (%s): %s\n",
           pemap_logical_flag ? "logical indices" : "OS indices", pemap);
    }
    // if a pemap is provided
    if (pemap != NULL){
      int mycore = search_pemap(pemap, CmiMyPeGlobal());
      if (pemap_logical_flag) {
        if (CmiSetCPUAffinityLogical(mycore) == -1) CmiAbort("CmiSetCPUAffinityLogical failed");
      }
      else {
        if (CmiSetCPUAffinity(mycore) == -1) CmiAbort("CmiSetCPUAffinity failed!");
      }
      if (show_affinity_flag) {
        CmiPrintf("Charm++> set PE %d on node %d to PU %c#%d\n", CmiMyPe(), CmiMyNode(),
            pemap_logical_flag ? 'L' : 'P', mycore);
      }
    }
    // if we are just using +setcpuaffinity
    else {
      CmiPrintf("Charm++> +setcpuaffinity implementation in progress\n");
    }
    #endif
    CmiNodeAllBarrier();
}

int CmiSetCPUAffinityLogical(int mycore)
{
  int core = mycore;
  if (core < 0) {
    core = CmiHwlocTopologyLocal.num_pus + core;
  }
  if (core < 0) {
    CmiError("Error: Invalid parameter to CmiSetCPUAffinityLogical: %d\n", mycore);
    CmiAbort("CmiSetCPUAffinityLogical failed!");
  }

  int thread_unitcount = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
  int thread_assignment = core % thread_unitcount;

  hwloc_obj_t thread_obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_PU, thread_assignment);

  int result = -1;

  if (thread_obj != nullptr)
  {
    result = set_thread_affinity(thread_obj->cpuset);
    //CpvAccess(myCPUAffToCore) = thread_obj->os_index;
  }

  if (result == -1)
    CmiError("Error: CmiSetCPUAffinityLogical failed to bind PE #%d to PU L#%d.\n", CmiMyPe(), mycore);

  return result;
}

// Uses PU indices assigned by the OS
int CmiSetCPUAffinity(int mycore) {
  #if defined(CPU_OR)
  int core = mycore;
  if (core < 0) {
    printf("Error with core number");
    CmiAbort("CmiSetCPUAffinity failed!");
  }
  /*
    if (core < 0) {
      core = CmiNumCores() + core;
    }
    if (core < 0) {
      CmiError("Error: Invalid parameter to CmiSetCPUAffinity: %d\n", mycore);
      CmiAbort("CmiSetCPUAffinity failed!");
    }
  */
  //  CpvAccess(myCPUAffToCore) = core;

  hwloc_obj_t thread_obj = hwloc_get_pu_obj_by_os_index(topology, core);

  int result = -1;

  if (thread_obj != nullptr)
    result = set_thread_affinity(thread_obj->cpuset);

  if (result == -1)
    printf("Error: CmiSetCPUAffinity failed to bind PE #%d to PU P#%d.\n",
           CmiMyPe(), mycore);

  return result;
  #else
  return -1;
  #endif
}

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

#else
// Dummy function if RECONVERSE_ENABLE_CPU_AFFINITY not set
void CmiInitCPUAffinity(char **argv) {}

void CmiCheckAffinity(void) {}

#endif

int CmiOnCore(void)
{
  printf("WARNING: CmiOnCore IS NOT SUPPORTED ON THIS PLATFORM\n");
  return -1;
}
