#ifndef RECONVERSE_COMM_BACKEND_H
#define RECONVERSE_COMM_BACKEND_H

#include <cstddef>
#include <vector>

#define MEMPOOL_INIT_SIZE_MB_DEFAULT   32
#define MEMPOOL_EXPAND_SIZE_MB_DEFAULT 64
#define MEMPOOL_MAX_SIZE_MB_DEFAULT    512
#define MEMPOOL_LB_DEFAULT             0
#define MEMPOOL_RB_DEFAULT             134217728

namespace comm_backend {

struct Status {
  const void *local_buf;
  size_t size;
  void *user_context;
};
using CompHandler = void (*)(Status status);
using AmHandler = int;
using mr_t = void *;
const mr_t MR_NULL = nullptr;

/**
 * @brief Initialize the communication backend. Not thread-safe.
 */
void init(char **argv);
/**
 * @brief Finalize the communication backend. Not thread-safe.
 */

void init_mempool();

void exit();
/**
 * @brief Initialize the communication backend for a new thread. Not
 * thread-safe.
 */
void initThread(int thread_id, int num_threads);
/**
 * @brief Finalize the communication backend for a thread. Not thread-safe.
 */
void exitThread();
/**
 * @brief Get the node ID of the current process. Thread-safe.
 */
int getMyNodeId();
/**
 * @brief Get the number of nodes in the system. Thread-safe.
 */
int getNumNodes();
/**
 * @brief Check if the backend supports RMA operations. Thread-safe.
 */
bool isRMACapable();
/**
 * @brief Register an active message handler. Not thread-safe.
 */
AmHandler registerAmHandler(CompHandler handler);
/**
 * @brief Issue an active message. Thread-safe.
 */
void issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
             CompHandler localComp, AmHandler remoteComp, void *user_context);
/**
 * @brief Issue a remote get operation. Thread-safe.
 */
void issueRget(int rank, const void *local_buf, size_t size, mr_t local_mr,
               void* remote_buf, void *rmr, CompHandler localComp, void *user_context);
/**
 * @brief Issue a remote put operation. Thread-safe.
 */
void issueRput(int rank, const void *local_buf, size_t size, mr_t local_mr,
               uintptr_t remote_disp, void *rmr, CompHandler localComp, void *user_context);
/**
 * @brief Make progress on the communication backend. Thread-safe.
 */
bool progress(void);
/**
 * @brief Block until all nodes have reached this point. Thread-safe.
 */
void barrier(void);
/**
 * @brief Register a memory region
 */
mr_t registerMemory(void *addr, size_t size);
/**
 * @brief Serialize (the remote handle of) the memory region into memory buffer
 * @param mr Memory region to serialize
 * @param addr Address to write the serialized data
 * @param size Maximum size of the buffer
 * @return The number of bytes written to the buffer, or will be written to the
 * buffer if the size is not enough
 */
size_t getRMR(mr_t mr, void *addr, size_t size);
/**
 * @brief Deregister a memory region
 */
void deregisterMemory(mr_t mr);

void *malloc(int nbytes, int header);

void free(void* msg);

/* ---------------------------------------------------------------------------
 * No-restart shrink/expand hooks.
 *
 * A rescale changes the set of processes in the job while every surviving
 * process keeps running. The backend is responsible for the transport half of
 * that: agreeing on the new membership, tearing down connections to departing
 * peers, building connections to arriving ones, and renumbering itself so that
 * node id n after the change addresses the n-th member of the new view.
 *
 * A backend that cannot do this reports supportsRescale() == false, and
 * ConverseCleanup aborts with a diagnostic rather than corrupting the job.
 * ------------------------------------------------------------------------- */

/**
 * @brief One member of the cluster, as the coordinator sees it.
 *
 * `addr` is the backend's own address blob for this member (a UCX worker
 * address, for instance). It is opaque to everything above the backend.
 */
struct Member {
  int nodeId;                       // position in the current view
  std::vector<unsigned char> addr;  // backend-specific wireup address
};

/**
 * @brief The membership of the job at a given epoch.
 */
struct ClusterView {
  unsigned int epoch;            // monotonically increasing membership epoch
  int nodeId;                    // this process's id in this view
  std::vector<Member> members;   // in nodeId order
};

/**
 * @brief Block until every operation this process posted has completed.
 *
 * Required before reconfigure: an outstanding send names an address that the
 * rebuild is about to remove.
 */
void drain(void);

/**
 * @brief Whether this backend implements the rescale hooks below. Thread-safe.
 */
bool supportsRescale(void);

/**
 * @brief This process's wireup address, for registering with the coordinator.
 *
 * Only meaningful when supportsRescale() is true.
 */
std::vector<unsigned char> getMyAddress(void);

/**
 * @brief The membership this process is currently wired up to, in nodeId order.
 */
const std::vector<Member> &getMembers(void);

/**
 * @brief Bootstrap from a coordinator rather than from the launcher's PMI.
 *
 * Called before init() when the launcher supplied +coordinator. Returns false
 * if the backend cannot bootstrap this way, in which case init() proceeds
 * normally.
 */
bool coordBootstrap(const char *coordHost, int coordPort, int myNodeId,
                    int numNodes, bool isNewcomer);

/**
 * @brief Reconfigure onto a new membership, in place.
 *
 * Closes connections to members that are gone, keeps the ones that survived,
 * opens connections to the ones that arrived, and adopts view.nodeId as this
 * process's new id. Called on every surviving process once the coordinator has
 * committed the view; departing processes exit instead of calling this.
 */
void reconfigure(const ClusterView &view);

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_H
