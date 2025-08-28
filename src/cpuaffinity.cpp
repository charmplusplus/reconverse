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
#if RECONVERSE_ENABLE_CPU_AFFINITY ON
#include "hwloc.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

CmiHwlocTopology CmiHwlocTopologyLocal;

// topology is the set of resources available to this process
// legacy_topology includes resources disallowed by the system, to implement
// CmiNumCores
static hwloc_topology_t topology, legacy_topology;

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
  hwloc_topology_set_flags(legacy_topology,
                           hwloc_topology_get_flags(legacy_topology) |
                               HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
  hwloc_topology_load(legacy_topology);

  depth = hwloc_get_type_depth(legacy_topology, HWLOC_OBJ_PU);
  CmiHwlocTopologyLocal.total_num_pus =
      depth != HWLOC_TYPE_DEPTH_UNKNOWN
          ? hwloc_get_nbobjs_by_depth(legacy_topology, depth)
          : 1;
}

static int set_thread_affinity(hwloc_cpuset_t cpuset) {
  pthread_t thread = pthread_self();

  if (hwloc_set_thread_cpubind(topology, thread, cpuset,
                               HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT)) {
    char *str;
    int error = errno;
    hwloc_bitmap_asprintf(&str, cpuset);
    printf("HWLOC> Couldn't bind to cpuset %s: %s\n", str, strerror(error));
    free(str);
    return -1;
  }

  return 0;
}

// Uses PU indices assigned by the OS
int CmiSetCPUAffinity(int mycore) {
  int core = mycore;
  if (core < 0) {
    printf("Error with core number");
    return -1;
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
}
#else
int CmiSetCPUAffinity(int mycore) {
  return 0;
}
void CmiInitHwlocTopology(void) {}

#endif

#else
int CmiSetCPUAffinity(int mycore) {
  return 0;
}
void CmiInitHwlocTopology(void) {}
#endif
