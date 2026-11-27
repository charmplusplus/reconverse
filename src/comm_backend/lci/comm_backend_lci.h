#ifndef RECONVERSE_COMM_BACKEND_LCI_H
#define RECONVERSE_COMM_BACKEND_LCI_H

#include "lci.hpp"
#include <atomic>
#include "comm_backend_internal.h"

#include <deque>
#include <mutex>
#include <unordered_set>

namespace comm_backend {
namespace lci_impl {
// A breach of the comm_backend interface with direct access to CmiAlloc/CmiFree
// There are another way to do this, but this is the simplest way to do it
struct AllocatorLCI : lci::allocator_base_t {
  // LCI asks for 0 bytes for a message without payload, which is what carries
  // the signal where LCI emulates a put with signal: the write, then a
  // zero-length message (cxi without FI_CXI_ENABLE_WRITEDATA, and
  // +lci_put_signal_emulate). CmiAlloc refuses 0 bytes: it prints "CmiAlloc:
  // size <= 0" and returns NULL, once per signal. Such a request gets a
  // shared empty buffer instead, which nothing writes (LCI copies 0 bytes
  // into it) and deallocate ignores.
  static void *emptyBuffer() {
    static char empty[1];
    return empty;
  }
  void *allocate(size_t size) override {
    return size == 0 ? emptyBuffer() : CmiAlloc(size);
  }

#if RECONVERSE_LCI_ALLOCATE_WITH_MR
  // A rendezvous AM's receive buffer, with the registration its pool block
  // already has on `device`. Left out for an LCI without the hook (see
  // CMakeLists.txt in this directory); LCI then registers each buffer itself.
  void *allocate_with_mr(size_t size, lci::device_t device,
                         lci::mr_t *mr) override;
#endif

  void deallocate(void *ptr) override {
    if (ptr != nullptr && ptr != emptyBuffer()) CmiFree(ptr);
  }
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
  bool isPutSignalCapable() override { return m_put_signal_capable; }
  bool isPutSignalNative() override {
    return m_put_signal_capable && m_put_signal_native && !m_put_signal_emulate;
  }
  int registerPutSignalKind(PutSignalHandler handler, void *ctx,
                            int nranks) override;
  void issueRputSignal(int rank, const void *local_buf, size_t size,
                       mr_t local_mr, uintptr_t remote_disp, void *rmr,
                       int dest_rank, int kind, uint16_t signal,
                       CompHandler localComp, void *user_context) override;
  // +lci_put_signal_emulate: the zero-length message that carries a signal
  // once its put has completed
  void sendEmulatedSignal(int rank, int dev, lci::rcomp_t rcomp,
                          uint16_t signal);
  bool progress(void) override;
  bool threadMustPoll() override;
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
  // Put with signal. Each kind has one rcomp per local rank, so the 16-bit
  // signal names one of 65536 entries of that rank, and the rcomp says which
  // rank and kind. Behind each rcomp is a PutSignalComp, which knows both;
  // the signal is the put's tag, carried with the rcomp in the immediate
  // data of a write with immediate data (ibv, ofi fi_writedata), or of the
  // zero-length message that follows the write where LCI emulates it
  // (support_putimm false). Kinds are published once, before any put of
  // theirs, so issueRputSignal reads them without a lock.
  static constexpr int kMaxPutSignalKinds = 64;
  struct PutSignalKind {
    std::vector<lci::comp_t> comps;    // per local rank
    std::vector<lci::rcomp_t> rcomps;  // per local rank
  };
  std::atomic<PutSignalKind *> m_put_signal_kinds[kMaxPutSignalKinds] = {};
  std::atomic<int> m_num_put_signal_kinds{0};
  bool m_put_signal_capable = false;
  // The provider has write with immediate data (LCI's support_putimm);
  // otherwise LCI emulates it
  bool m_put_signal_native = false;
  // +lci_put_signal_emulate: reconverse posts the write, and after its
  // completion the zero-length message, as LCI does without support_putimm,
  // so that path can be tested where the provider has write with immediate
  // data
  bool m_put_signal_emulate = false;
  lci::comp_t m_emulated_signal_comp;
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

  // An AM larger than m_maxEager goes by LCI's rendezvous protocol, and holds
  // a packet of the pool receives are posted from until the destination's
  // ready-to-receive arrives and the data is written. So that such sends can
  // never take the packets the receives need (issue #257), at most m_rdvMax
  // are in flight (detail::g_rdv_inflight); the rest wait in m_pending, oldest
  // first, and progress() posts them as earlier ones complete.
  struct PendingAm {
    int rank;
    const void *local_buf;
    size_t size;
    mr_t mr;
    CompHandler localComp;
    AmHandler remoteComp;
    void *user_context;
  };
  size_t m_maxEager = 0;
  long m_rdvMax = 0;
  std::mutex m_pendingMutex;
  std::deque<PendingAm> m_pending;
  // m_pending.size(), read without the lock; drops only once a send is posted
  alignas(CMI_CACHE_LINE_SIZE) std::atomic<size_t> m_nPending{0};
  bool postRendezvous(const PendingAm &am, bool spin);
  void postPending();

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
