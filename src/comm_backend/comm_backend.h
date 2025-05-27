#ifndef RECONVERSE_COMM_BACKEND_H
#define RECONVERSE_COMM_BACKEND_H

#include <cstddef>

#define MEMPOOL_INIT_SIZE_MB_DEFAULT   64
#define MEMPOOL_EXPAND_SIZE_MB_DEFAULT 64
#define MEMPOOL_MAX_SIZE_MB_DEFAULT    512
#define MEMPOOL_LB_DEFAULT             0
#define MEMPOOL_RB_DEFAULT             134217728

namespace comm_backend {

struct Status {
  void *msg;
  size_t size;
};
using CompHandler = void (*)(Status status);
using AmHandler = int;
using mr_t = void *;
const mr_t MR_NULL = nullptr;

/**
 * @brief Initialize the communication backend. Not thread-safe.
 */
void init(int *argc, char ***argv);
/**
 * @brief Finalize the communication backend. Not thread-safe.
 */
void exit();
/**
 * @brief Get the node ID of the current process. Thread-safe.
 */
int getMyNodeId();
/**
 * @brief Get the number of nodes in the system. Thread-safe.
 */
int getNumNodes();
/**
 * @brief Register an active message handler. Not thread-safe.
 */
AmHandler registerAmHandler(CompHandler handler);
/**
 * @brief Send an active message. Thread-safe.
 */
void sendAm(int rank, void *msg, size_t size, mr_t mr, CompHandler localComp,
            AmHandler remoteComp);
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
 * @brief Deregister a memory region
 */
void deregisterMemory(mr_t mr);

void *malloc(int nbytes, int header);

void free(void* msg);

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_H
