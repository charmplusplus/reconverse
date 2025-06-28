#ifndef RECONVERSE_COMM_BACKEND_H
#define RECONVERSE_COMM_BACKEND_H

#include <cstddef>

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
 * @brief Issue an active message. Thread-safe.
 */
void issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
             CompHandler localComp, AmHandler remoteComp, void *user_context);
/**
 * @brief Issue a remote get operation. Thread-safe.
 */
void issueRget(int rank, const void *local_buf, size_t size, mr_t local_mr,
               uintptr_t remote_disp, void *rmr, CompHandler localComp, void *user_context);
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

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_H
