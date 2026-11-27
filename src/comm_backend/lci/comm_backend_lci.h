#ifndef RECONVERSE_COMM_BACKEND_LCI_H
#define RECONVERSE_COMM_BACKEND_LCI_H

#include "lci.hpp"
#include <atomic>
#include "comm_backend_internal.h"

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace comm_backend {
namespace lci_impl {
// A breach of the comm_backend interface with direct access to CmiAlloc/CmiFree
// There are another way to do this, but this is the simplest way to do it
struct AllocatorLCI : lci::allocator_base_t {
  void *allocate(size_t size) override { return CmiAlloc(size); }

#if RECONVERSE_LCI_ALLOCATE_WITH_MR
  // A rendezvous AM's receive buffer, with the registration its pool block
  // already has on `device`. Left out for an LCI without the hook (see
  // CMakeLists.txt in this directory); LCI then registers each buffer itself.
  void *allocate_with_mr(size_t size, lci::device_t device,
                         lci::mr_t *mr) override;
#endif

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
  void flushSends() override;
  size_t pendingSends(size_t *bytes) override;
  void barrier(void) override;
  mr_t registerMemory(void *addr, size_t size) override;
  // registerMemory, then registered on every device at once
  mr_t registerMemoryAllDevices(void *addr, size_t size);
  // The registration of a region on one of our devices, for LCI to receive into
  lci::mr_t getMRForDevice(mr_t mr, lci::device_t device);
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
  void releaseRegion(mr_t mr);

  bool m_remote_free = true;  // off with +nomempool_remote_free

  // An operation LCI did not take (it answered retry m_postTries times, or
  // an AM larger than m_maxEager found m_rdvMax of them in flight already)
  // waits in m_pending, and the caller goes on. Each destination rank has its
  // own queue, oldest first, and an operation for a rank with a nonempty
  // queue joins it, so it cannot overtake the ones waiting. progress() posts
  // from the queues. See README.md, "Sends that LCI cannot take at once".
  struct PendingOp {
    enum Kind : uint8_t { AM, GET, PUT } kind;
    int rank;
    int dev;  // index of the device it is posted on
    const void *local_buf;
    size_t size;
    mr_t mr;
    CompHandler localComp;
    AmHandler remoteComp;   // AM
    void *user_context;
    uintptr_t remote_disp;  // GET, PUT
    lci::rmr_t rmr;         // GET, PUT
  };
  enum class PostResult { posted, done, retry, locked, capped };
  size_t m_maxEager = 0;
  long m_rdvMax = 0;
  int m_postTries = 4;  // +lci_post_tries
  std::mutex m_pendingMutex;
  std::unordered_map<int, std::deque<PendingOp>> m_pending;  // by rank
  // The ranks with a queue, in the order postPending() tries them next
  std::deque<int> m_pendingRanks;
  // Read without the lock on the send path: operations waiting for each
  // rank, in all, and their bytes. A count drops only once one is posted.
  std::unique_ptr<std::atomic<uint32_t>[]> m_pendingOfRank;
  alignas(CMI_CACHE_LINE_SIZE) std::atomic<size_t> m_nPending{0};
  std::atomic<size_t> m_pendingBytes{0};
  PostResult postOp(const PendingOp &op);
  void submit(const PendingOp &op);
  void enqueue(const PendingOp &op);
  void postPending();
  int getThreadLocalDeviceIndex();

  lci::device_t getThreadLocalDevice();
  lci::mr_t getThreadLocalMR(mr_t mr);
  lci::mr_t getDeviceMR(mr_t mr, int device_idx);
  bool progressDevice(int device_idx);
  // Progress the caller's device and, if different, the one it posts on.
  void progressForPost(int device_idx);
  int checkedDeviceIndex(uint32_t device_idx);
};

} // namespace lci_impl
} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI_H
