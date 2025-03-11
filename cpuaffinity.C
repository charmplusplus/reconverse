#include <stdio.h>
#include "hwloc.h"
#include <CpvMacros.h>
#include "converse.h"

CpvDeclare(int, myCPUAffToCore);

CmiHwlocTopology CmiHwlocTopologyLocal;

// topology is the set of resources available to this process
// legacy_topology includes resources disallowed by the system, to implement CmiNumCores
static hwloc_topology_t topology, legacy_topology;

void CmiInitHwlocTopology(void)
{
    int depth;

    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);

    // packages == sockets
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PACKAGE);
    CmiHwlocTopologyLocal.num_sockets = depth != HWLOC_TYPE_DEPTH_UNKNOWN ? hwloc_get_nbobjs_by_depth(topology, depth) : 1;

    // cores
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_CORE);
    CmiHwlocTopologyLocal.num_cores = depth != HWLOC_TYPE_DEPTH_UNKNOWN ? hwloc_get_nbobjs_by_depth(topology, depth) : 1;

    // PUs
    depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PU);
    CmiHwlocTopologyLocal.num_pus = depth != HWLOC_TYPE_DEPTH_UNKNOWN ? hwloc_get_nbobjs_by_depth(topology, depth) : 1;


    // Legacy: Determine the system's total PU count

    hwloc_topology_init(&legacy_topology);
    hwloc_topology_set_flags(legacy_topology, hwloc_topology_get_flags(legacy_topology) | HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
    hwloc_topology_load(legacy_topology);

    depth = hwloc_get_type_depth(legacy_topology, HWLOC_OBJ_PU);
    CmiHwlocTopologyLocal.total_num_pus =
      depth != HWLOC_TYPE_DEPTH_UNKNOWN ? hwloc_get_nbobjs_by_depth(legacy_topology, depth) : 1;
}

/* called in ConverseCommonInit to initialize basic variables */
void CmiInitCPUAffinityUtil(void){
    CpvInitialize(int, myCPUAffToCore);
    CpvAccess(myCPUAffToCore) = -1;
}


static int set_thread_affinity(hwloc_cpuset_t cpuset)
{
  pthread_t thread = pthread_self();

  if (hwloc_set_thread_cpubind(topology, thread, cpuset, HWLOC_CPUBIND_THREAD|HWLOC_CPUBIND_STRICT))
  {
    char *str;
    int error = errno;
    hwloc_bitmap_asprintf(&str, cpuset);
    printf("HWLOC> Couldn't bind to cpuset %s: %s\n", str, strerror(error));
    free(str);
    return -1;
  }

  return 0;
}


// Uses logical PU indices as determined by hwloc
int CmiSetCPUAffinityLogical(int mycore)
{
  int core = mycore;
  if (core < 0) {
    core = CmiHwlocTopologyLocal.num_pus + core;
  }
  if (core < 0) {
    printf("Error: Invalid parameter to CmiSetCPUAffinityLogical: %d\n", mycore);
    printf("CmiSetCPUAffinityLogical failed!");
  }

  int thread_unitcount = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
  int thread_assignment = core % thread_unitcount;

  hwloc_obj_t thread_obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_PU, thread_assignment);

  int result = -1;

  if (thread_obj != nullptr)
  {
    result = set_thread_affinity(thread_obj->cpuset);
    CpvAccess(myCPUAffToCore) = thread_obj->os_index;
  }

  if (result == -1)
    printf("Error: CmiSetCPUAffinityLogical failed to bind PE #%d to PU L#%d.\n", CmiMyPe(), mycore);

  return result;
}
