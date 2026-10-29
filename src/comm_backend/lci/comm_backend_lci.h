#ifndef RECONVERSE_COMM_BACKEND_LCI_H
#define RECONVERSE_COMM_BACKEND_LCI_H

#include "lci.hpp"
#include <atomic>
#include "comm_backend_internal.h"

#include <mutex>
#include <unordered_set>

namespace comm_backend {
namespace lci_impl {
// A breach of the comm_backend interface with direct access to CmiAlloc/CmiFree
// There are another way to do this, but this is the simplest way to do it
struct AllocatorLCI : lci::allocator_base_t {
  void *allocate(size_t size) override { return CmiAlloc(size); }

  void deallocate(void *ptr) override { CmiFree(ptr); }
};

struct MempoolOptions {
  size_t mempool_init_size;
  size_t mempool_expand_size;
  long long mempool_max_size;
  size_t mempool_lb_size;
  size_t mempool_rb_size;
};

static MempoolOptions mempool_options = {
  MEMPOOL_INIT_SIZE_MB_DEFAULT * ONE_MB,
  MEMPOOL_EXPAND_SIZE_MB_DEFAULT * ONE_MB,
  MEMPOOL_MAX_SIZE_MB_DEFAULT * ONE_MB,
  MEMPOOL_LB_DEFAULT,
  MEMPOOL_RB_DEFAULT
};

class CommBackendLCI : public CommBackendBase {
public:
  void init(char **argv) override;
  void init_mempool() override;
  void exit() override;
  void abortAll(int exit_code, const char *msg) override;
  void initThread(int thread_id, int num_threads) override;
  void exitThread() override;
  int getMyNodeId() override;
  int getNumNodes() override;
  bool isRMACapable() override { return true; }
  AmHandler registerAmHandler(CompHandler handler) override;
  void issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
               CompHandler localComp, AmHandler remoteComp, void *user_context) override;
  void issueRget(int rank, const void *local_buf, size_t size, mr_t local_mr,
                 void* remote_buf, void *rmr,
                 CompHandler localComp, void *user_context) override;
  void issueRput(int rank, const void *local_buf, size_t size, mr_t local_mr,
                 uintptr_t remote_disp, void *rmr,
                 CompHandler localComp, void *user_context) override;
  bool progress(void) override;
  bool threadMustPoll() override;
  void barrier(void) override;
  mr_t registerMemory(void *addr, size_t size) override;
  size_t getRMR(mr_t mr, void *addr, size_t size) override;
  void deregisterMemory(mr_t mr) override;

  void *malloc(int n_bytes, int header) override;
  void free(void* msg) override;
private:
  struct threadContext {
    int thread_id;
    lci::device_t device;
  };

  std::vector<lci::device_t> m_devices;
  // One trylock per device: prevents concurrent fi_cq_read calls on the same CQ
  // (OFI fi_cq_read is not thread-safe without FI_THREAD_SAFE domain).
  // Each lock on its own cache line, so polling one device does not
  // invalidate the line holding the other devices' locks.
  struct alignas(CMI_CACHE_LINE_SIZE) PaddedLock {
    std::atomic<bool> v{false};
  };
  std::vector<PaddedLock> m_progress_locks;
  lci::comp_t m_local_comp;
  lci::comp_t m_remote_comp;
  lci::rcomp_t m_rcomp;
  AllocatorLCI m_allocator;

  // Memory regions handed out by registerMemory() that have not been given back
  // to deregisterMemory(). A buffer registered with CK_BUFFER_NODEREG is never
  // deregistered by the application, so without this the region is still open
  // when exit() closes the device and fi_close(domain) fails with FI_EBUSY.
  std::mutex m_liveMrsMutex;
  std::unordered_set<void *> m_liveMrs;
  void deregisterAllMemory();

  bool m_remote_free = true;  // off with +nomempool_remote_free

  lci::device_t getThreadLocalDevice();
  lci::mr_t getThreadLocalMR(mr_t mr);
  lci::rmr_t getThreadLocalRMR(void *rmr);
};

} // namespace lci_impl
} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI_H
