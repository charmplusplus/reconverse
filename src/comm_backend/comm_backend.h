#ifndef RECONVERSE_COMM_BACKEND_H
#define RECONVERSE_COMM_BACKEND_H

#include <cstddef>

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
 * @brief Ask the launcher to end every process of the job, as MPI_Abort does.
 * Called from CmiAbort before this process aborts; a no-op when the backend
 * has no process manager to ask (lcrun's file bootstrap, single process).
 */
void abortAll(int exit_code, const char *msg);
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
               void *remote_buf, void *rmr, CompHandler localComp, void *user_context);
/**
 * @brief A remote put whose completion at the target raises the registered
 * signal handler with the 16-bit tag and the bytes written (RDMA write with
 * immediate data). Thread-safe.
 */
using SignalHandler = void (*)(int rank, uint32_t tag, size_t size);
void setSignalHandler(SignalHandler handler);
void issueRputSignal(int rank, const void *local_buf, size_t size, mr_t local_mr,
                     void *remote_buf, void *rmr, uint32_t tag, CompHandler localComp,
                     void *user_context);
/**
 * @brief Make progress on the communication backend. Thread-safe.
 */
bool progress(void);
/**
 * @brief Whether the calling thread must poll progress() regardless of
 * +backend_poll_thread (e.g. it is the only thread of its device that would).
 * Call after initThread.
 */
bool threadMustPoll(void);
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

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_H
