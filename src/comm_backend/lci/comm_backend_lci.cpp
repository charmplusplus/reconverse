#include "mempool.h"
#include "lci.hpp"
#include "lct.h"
#include "comm_backend/lci/comm_backend_lci.h"
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

CpvDeclare(mempool_type*, mempool);

namespace comm_backend {
namespace lci_impl {

namespace detail {
struct ThreadContext {
  int thread_id;
  int device_idx;
  lci::device_t tls_device;
  // The first thread of each device: progress() only polls the caller's own
  // device, so a device none of whose threads poll is never progressed
  bool device_poller;
};

thread_local ThreadContext g_thread_context;

// Set while a thread is inside lci::progress_x(), i.e. possibly in a
// completion callback, with its device's progress lock held. An operation
// issued then is tried once and queued if LCI does not take it: a retry loop
// could not poll the device.
thread_local bool g_in_progress_callback = false;

// Set while a thread is in postPending(), so that an operation issued by a
// completion handler it calls does not start another pass.
thread_local bool g_in_post_pending = false;

// Rendezvous AMs posted and not yet completed (CommBackendLCI::m_rdvMax).
alignas(CMI_CACHE_LINE_SIZE) std::atomic<long> g_rdv_inflight{0};
} // namespace detail

std::vector<CompHandler> g_handlers;

struct localCallbackArgs {
  CompHandler handler;
  void *user_context;
  bool rdv = false;  // a rendezvous AM, counted in g_rdv_inflight
};

void localCallback(lci::status_t status) {
  auto args = reinterpret_cast<localCallbackArgs *>(status.get_user_context());
  auto handler = args->handler;
  auto user_context = args->user_context;
  if (args->rdv)
    detail::g_rdv_inflight.fetch_sub(1, std::memory_order_relaxed);
  delete args;
  handler({status.get_buffer(), status.get_size(), user_context});
}

void remoteCallback(lci::status_t status) {
  auto am_handler = static_cast<AmHandler>(status.get_tag());
  auto handler = g_handlers[am_handler];
  handler({status.get_buffer(), status.get_size(), nullptr});
}

// For alloc_mempool_block, which the mempool calls without a backend.
static CommBackendLCI *g_lci_backend = nullptr;

// What an mr_t from registerMemory() points to: the buffer, and one LCI
// registration per device, each made the first time a transfer asks for that
// device (getDeviceMR). A transfer reads one device's registration on each
// side, the device the buffer's owner exported (see WireRMR), so registering
// on every device up front cost +lci_ndevices registrations per buffer where
// one or two were used.
struct LazyMR {
  void *addr;
  size_t size;
  std::unique_ptr<std::atomic<lci::mr_impl_t *>[]> slots; // null: not yet

  LazyMR(void *addr_, size_t size_, size_t ndevices)
      : addr(addr_), size(size_),
        slots(new std::atomic<lci::mr_impl_t *>[ndevices]) {
    for (size_t i = 0; i < ndevices; i++)
      slots[i].store(nullptr, std::memory_order_relaxed);
  }
};

void *alloc_mempool_block(size_t *size, void **mem_hndl, int expand_flag)
{
    size_t alloc_size =  expand_flag ? mempool_options.mempool_expand_size : mempool_options.mempool_init_size;
    if (*size < alloc_size) *size = alloc_size;
    if (*size > mempool_options.mempool_max_size)
    {
        CmiPrintf("Error: there is attempt to allocate memory block with size %ld which is greater than the maximum mempool allowed %lld.\n"
                  "Please increase the maximum mempool size by using +ofi-mempool-max-size\n",
                  *size, mempool_options.mempool_max_size);
        CmiAbort("alloc_mempool_block");
    }

    void *pool = NULL;
    if (posix_memalign(&pool, ALIGNBUF, *size) != 0 || pool == NULL)
    {
        CmiAbort("alloc_mempool_block: posix_memalign failed");
    }
    // Keep the registration handle so the block can be deregistered at teardown.
    *mem_hndl = g_lci_backend->registerMemoryAllDevices(pool, *size);
    return pool;
}

// Set by exit() once deregisterAllMemory() has released every registration,
// the pool blocks' included; the pools are destroyed after that, and their
// handles are stale by then.
static bool g_pool_mrs_released = false;

void free_mempool_block(void *ptr, void* mem_hndl)
{
    // Deregister the MR before releasing the host pages it covers.
    if (mem_hndl != NULL && !g_pool_mrs_released)
        deregisterMemory((mr_t) mem_hndl);
    // Use ::free explicitly: unqualified `free` here resolves to comm_backend::free,
    // which expects a CmiChunkHeader-prefixed user pointer. This function receives
    // a raw posix_memalign block, so we need the C standard library free.
    ::free(ptr);
}

void CommBackendLCI::init_mempool()
{
  CpvInitialize(mempool_type*, mempool);

  CpvAccess(mempool) = mempool_init(mempool_options.mempool_init_size,
    alloc_mempool_block,
    free_mempool_block,
    mempool_options.mempool_max_size);
}

void* CommBackendLCI::malloc(int n_bytes, int header)
{
  char *ptr = NULL;
  size_t size = n_bytes + header;
  if (size <= mempool_options.mempool_lb_size)
  {
    CmiAbort("OFI pool lower boundary violation");
  }
  else
  {
    // Ensure mempool_header + CmiChunkHeader fit within ALIGNBUF
    CmiAssert(header+sizeof(mempool_header) <= ALIGNBUF);
    n_bytes=ALIGN64(n_bytes);
    if( n_bytes < BIG_MSG)
      {
        // Allocate ALIGNBUF for headers + n_bytes for user data
        char *res = (char *)mempool_malloc(CpvAccess(mempool), ALIGNBUF+n_bytes, 1);

        // res points to the mempool_header. We need to return a pointer
        // that points to where the CmiChunkHeader is located.
        // Layout: [mempool_header][CmiChunkHeader][user data]
        // So CmiChunkHeader starts at: res + sizeof(mempool_header)
        if (res)
        {
          ptr = res + sizeof(mempool_header);
          // Record the pool block's registration handle in the chunk header so
          // inter-node sends pass a valid MR to LCI. Otherwise MRFIELD(msg) reads
          // uninitialized bytes from a reused slot and getThreadLocalMR()
          // dereferences a garbage pointer.
          ((CmiChunkHeader *)ptr)->mr = GetMemHndl(res);
        }
      }
    else
      {
        // For out-of-pool, we need space for out_of_pool_header which contains
        // block_header and mempool_header, then CmiChunkHeader, then user data
        n_bytes = size + sizeof(out_of_pool_header);
        n_bytes = ALIGN64(n_bytes);
        char *res;

        posix_memalign((void **)&res, ALIGNBUF, n_bytes);
        out_of_pool_header *mptr= (out_of_pool_header*) res;
        // construct the minimal version of the
        // mempool_header+block_header like a memory pool message
        // so that all messages can be handled the same way with
        // the same macros and functions.  We need the mptr,
        // block_ptr, and mem_hndl fields and can test the size to
        // know to not put it back in the normal pool on free
        mr_t mr = registerMemory(res, n_bytes);
        mptr->block_head.mem_hndl=mr;
        mptr->block_head.mptr=(struct mempool_type*) res;
        mptr->block.block_ptr=(struct block_header *)res;
        // Return pointer to where CmiChunkHeader should be placed
        // Layout: [out_of_pool_header = block_header + mempool_header][CmiChunkHeader][user data]
        ptr=(char *) res + sizeof(out_of_pool_header);
        // Same as the pooled path: record the MR for inter-node sends.
        ((CmiChunkHeader *)ptr)->mr = mr;
      }
    }

  if (!ptr) CmiAbort("LrtsAlloc");
  return ptr;
}

void CommBackendLCI::free(void *msg)
{
  int headersize = sizeof(CmiChunkHeader);
  // msg points to user data, we need to go back to find mempool_header
  // Layout: [mempool_header][CmiChunkHeader][user data (msg)]
  char *aligned_addr = (char *)msg - headersize - sizeof(mempool_header);
  size_t size = static_cast<size_t>(SIZEFIELD((char *)msg));
  if (size <= mempool_options.mempool_lb_size)
    CmiAbort("LCI: mempool lower boundary violation");
  else
    size = ALIGN64(size);
  if(size>=BIG_MSG)
  {
    // For out-of-pool messages, go back to out_of_pool_header
    // Layout: [out_of_pool_header][CmiChunkHeader][user data (msg)]
    deregisterMemory((mr_t)MRFIELD(msg));
    ::free((char *)msg - headersize - sizeof(out_of_pool_header));
  }
  else
  {
    // For in-pool messages, free from mempool_header position. A block of
    // another PE's pool (a receive buffer this PE did not drain, a message
    // another PE allocated) goes back without taking that pool's lock
    if (m_remote_free) {
      mempool_type* owner = mempool_owner(aligned_addr);
      if (owner != CpvAccess(mempool)) {
        mempool_free_remote(owner, aligned_addr);
        return;
      }
    }
    mempool_free_thread(aligned_addr);
  }
}

void CommBackendLCI::init(char **argv) {
  int num_devices = 1;
  CmiGetArgInt(argv, "+lci_ndevices", (int *)&num_devices);
  CmiAssert(num_devices >= 1);
  // On unless +nomempool_remote_free; +mempool_remote_free is still accepted,
  // from launch lines written before it was the default.
  CmiGetArgFlagDesc(argv, "+mempool_remote_free",
      "free blocks of another PE's message pool through a lock-free list it drains, not under its lock (default)");
  m_remote_free = !CmiGetArgFlagDesc(argv, "+nomempool_remote_free",
      "free blocks of another PE's message pool under its lock");

  // Slingshot (cxi): hardware RX matching aborts once NIC list entries run out
  // ("LE resources not recovered"); hybrid falls back to software matching.
  setenv("FI_CXI_RX_MATCH_MODE", "hybrid", 0);
  // cxi sends a message larger than FI_CXI_RDZV_THRESHOLD plus
  // FI_CXI_RDZV_GET_MIN (18433 bytes by default) with a rendezvous of its own,
  // which made an eager LCI message above that 2-3 us slower on Delta (issue
  // #334). An eager LCI message fits in a packet, so with packets larger than
  // 16 KiB the threshold is raised to the packet size, unless the environment
  // already sets it. cxi reads it during lci::global_initialize() (LCI's OFI
  // probe), before LCI's attributes can be asked for, so the packet size is
  // read here the way LCI reads it.
  // cxi reads its variables once per process, so neither setting here has an
  // effect if another library (Cray MPICH's MPI_Init, say) started libfabric
  // first.
  const char *packetSizeEnv = getenv("LCI_ATTR_PACKET_SIZE");
  int packetSize = packetSizeEnv ? atoi(packetSizeEnv) : LCI_PACKET_SIZE_DEFAULT;
  const char *rdzvThresholdEnv = getenv("FI_CXI_RDZV_THRESHOLD");
  std::string lowRdzvThreshold;  // set when the environment's value is below the packet size
  if (packetSize > 16384) {
    if (rdzvThresholdEnv == nullptr)
      setenv("FI_CXI_RDZV_THRESHOLD", std::to_string(packetSize).c_str(), 0);
    else if (atol(rdzvThresholdEnv) < packetSize)
      lowRdzvThreshold = rdzvThresholdEnv;
  }

  lci::global_initialize();
  if (!lowRdzvThreshold.empty() && lci::get_rank_me() == 0)
    CmiPrintf("Reconverse> Warning: FI_CXI_RDZV_THRESHOLD=%s is below the LCI "
              "packet size (%d), so eager LCI messages larger than it plus "
              "FI_CXI_RDZV_GET_MIN use cxi's slower rendezvous (issue #334). "
              "Unset it to let reconverse raise it to the packet size.\n",
              lowRdzvThreshold.c_str(), packetSize);
  auto g_attr = lci::get_g_default_attr();
  // On OFI, libfabric caches registrations itself, and its memhooks monitor
  // replaces the munmap/madvise hooks LCI's cache relies on, so that cache
  // keeps stale registrations (cxil_map failures, stalled transfers).
  // LCI_ATTR_USE_REG_CACHE still overrides this.
  if (g_attr.backend == lci::attr_backend_t::ofi && g_attr.use_reg_cache &&
      getenv("LCI_ATTR_USE_REG_CACHE") == nullptr)
    g_attr.use_reg_cache = false;
  // We will make sure the total packet number is always at least twice as large as the total preposted receives.
  // We also set a minimum of 1024 preposted receives per device and increase the total number of packets if needed.
  if (g_attr.net_max_recvs * num_devices > g_attr.npackets / 2) {
    g_attr.net_max_recvs = g_attr.npackets / 2 / num_devices;
    if (g_attr.net_max_recvs < 1024) {
      g_attr.net_max_recvs = 1024;
      g_attr.npackets = 1024 * num_devices * 2;
    }
  }
  // SMP mode has multiple PE threads per node sharing one device. The CXI
  // provider is not thread-safe without FI_THREAD_SAFE, so enable LCI's
  // built-in per-device trylock for all OFI operations.
  g_attr.ofi_lock_mode = lci::LCI_NET_TRYLOCK_SEND |
                         lci::LCI_NET_TRYLOCK_RECV |
                         lci::LCI_NET_TRYLOCK_POLL;
  lci::set_g_default_attr(g_attr);

  lci::g_runtime_init_x().alloc_default_device(false)();
  m_devices.resize(num_devices);
  m_progress_locks = std::vector<PaddedLock>(num_devices);
  for (int i = 0; i < num_devices; i++) {
    m_devices[i] = lci::alloc_device();
    m_progress_locks[i].v.store(false, std::memory_order_relaxed);
  }

  m_local_comp = lci::alloc_handler(localCallback);
  m_remote_comp = lci::alloc_handler(remoteCallback);
  m_rcomp = lci::register_rcomp(m_remote_comp);
  lci::set_allocator(&m_allocator);

  // Rendezvous sends in flight may hold half of the packets the posted
  // receives do not, so the receives can always be reposted (see issueAm).
  m_maxEager = lci::get_max_bcopy_size();
  m_rdvMax = ((long)g_attr.npackets -
              (long)g_attr.net_max_recvs * num_devices) / 2;
  int rdvMax = 0;
  if (CmiGetArgIntDesc(argv, "+lci_max_rdv_sends", &rdvMax,
                       "rendezvous-size sends a process keeps in flight; the "
                       "rest wait their turn (default: half the LCI packets "
                       "that are not preposted receives)"))
    m_rdvMax = rdvMax;
  if (m_rdvMax < 1) m_rdvMax = 1;
  CmiGetArgIntDesc(argv, "+lci_post_tries", &m_postTries,
                   "attempts to hand LCI a send, with progress in between, "
                   "before it is queued and the PE goes on (default: 16)");
  if (m_postTries < 1) m_postTries = 1;
  m_pendingOfRank.reset(new std::atomic<uint32_t>[lci::get_rank_n()]);
  for (int i = 0; i < lci::get_rank_n(); i++)
    m_pendingOfRank[i].store(0, std::memory_order_relaxed);
  g_lci_backend = this;
  barrier();
}

// The PMI abort reaches the process manager (Slurm's PMI2/PMIx, mpirun), which
// then ends the other ranks; without it they spin until the job's time limit
// (issue #285). lcrun's file bootstrap has no manager: there lcrun itself
// kills the peers when a process exits with a failure status.
void CommBackendLCI::abortAll(int exit_code, const char *msg) {
#ifdef LCT_PMI_HAS_ABORT
  if (LCT_pmi_initialized()) LCT_pmi_abort(exit_code, msg);
#else
  (void)exit_code;
  (void)msg;
#endif
}

void CommBackendLCI::exit() {
  // Operations still queued (ConverseExit's flushSends gave up, or they were
  // issued after it) are not delivered; give the messages' buffers back.
  for (auto &entry : m_pending)
    for (const PendingOp &op : entry.second)
      if (op.kind == PendingOp::AM)
        op.localComp({op.local_buf, op.size, op.user_context});
  m_pending.clear();
  m_pendingRanks.clear();
  m_nPending.store(0, std::memory_order_relaxed);
  m_pendingBytes.store(0, std::memory_order_relaxed);
  // A rendezvous AM's data is written using the registrations of pool blocks
  // on both ends (the send buffer's, and the receive buffer's through
  // AllocatorLCI::allocate_with_mr), which deregisterAllMemory releases. So
  // finish this process's rendezvous sends first, then wait until every
  // process has: a write completes only once its target has taken it, so
  // past the barrier none is outstanding anywhere. Without this, a process
  // that got here first released the pool while a slower one still wrote
  // into it, and the writer failed ("Network completion failed",
  // tests/exit_inflight on the OFI tcp provider). This thread has run
  // exitThread, so progress() and barrier() poll every device.
  // The barrier makes exit() collective, but adds no new condition: its only
  // caller is CmiNodeTeardown on rank 0, reached from ConverseExit or
  // ConverseInteropFinalize, and both come after ConverseExit's barrier,
  // which every process must already reach. A process that dies between
  // that barrier and this one leaves the others waiting here, unless the
  // launcher ends them when one process dies.
  while (detail::g_rdv_inflight.load(std::memory_order_acquire) > 0)
    progress();
  barrier();
  // Drop any still-registered memory regions before tearing the devices down;
  // an open region makes fi_close(domain) fail with FI_EBUSY. This supersedes
  // the reconverse_lci_cache_amd workaround of skipping free_device entirely.
  // The mempool blocks are registered through registerMemory too, so this
  // covers them; free_mempool_block below must not deregister them again.
  deregisterAllMemory();
  g_pool_mrs_released = true;
  for (auto device : m_devices) {
    lci::free_device(&device);  // drains and completes what is still posted
  }
  m_devices.clear();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
  lci::g_runtime_fina();
  lci::global_finalize();

  // Only now, with no network progress left anywhere, release every PE's
  // mempool. Until this point any thread's progress() may still complete a
  // send (CommLocalHandler frees the buffer into the sender's pool) or land a
  // receive (LCI allocates the buffer with CmiAlloc, i.e. from the polling
  // thread's pool), so no pool may go away while the devices are up. Rank 0
  // does it for all ranks; the others have run exitThread and no longer poll
  // (parked in ConverseExit, or, in return-on-exit mode, exited and joined).
  for (int r = 0; r < CmiMyNodeSize(); r++) {
    mempool_type *&pool = CpvAccessOther(mempool, r);
    if (pool != NULL) {
      mempool_destroy(pool);
      pool = NULL;
    }
  }
}

void CommBackendLCI::initThread(int thread_id, int num_threads) {
  detail::g_thread_context.thread_id = thread_id;
  int nthreads_per_device =
      (num_threads + m_devices.size() - 1) / m_devices.size();
  int device_id = thread_id / nthreads_per_device;
  CmiAssert(device_id < m_devices.size());
  detail::g_thread_context.device_idx = device_id;
  detail::g_thread_context.tls_device = m_devices[device_id];
  detail::g_thread_context.device_poller = thread_id % nthreads_per_device == 0;
}

void CommBackendLCI::exitThread() {
  // This PE's mempool is NOT destroyed here: completions processed later by
  // another thread's progress() still allocate from and free into it (see
  // exit()). Destroying it here freed buffers of in-flight sends and receive
  // buffers under LCI: SIGSEGV in CmiFree / CommRemoteHandler <- progress <-
  // ConverseExit, seen after broadcast-heavy runs and in tests/exit_inflight.
  detail::g_thread_context.tls_device = lci::device_t();
  detail::g_thread_context.device_idx = -1;
  detail::g_thread_context.thread_id = -1;
}

int CommBackendLCI::getMyNodeId() { return lci::get_rank_me(); }

int CommBackendLCI::getNumNodes() { return lci::get_rank_n(); }

AmHandler CommBackendLCI::registerAmHandler(CompHandler handler) {
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

int CommBackendLCI::getThreadLocalDeviceIndex() {
  return detail::g_thread_context.tls_device.is_empty()
             ? 0
             : detail::g_thread_context.device_idx;
}

void CommBackendLCI::issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
                              CompHandler localComp, AmHandler remoteComp, void *user_context) {
  submit(PendingOp{PendingOp::AM, rank, getThreadLocalDeviceIndex(), local_buf,
                   size, mr, localComp, remoteComp, user_context, 0,
                   lci::rmr_t()});
}

// One attempt to hand an operation to LCI. `done`: it completed in the call
// and the caller owes op.localComp. `retry`: LCI did not take it. `locked`:
// nor did it, because another thread was posting on the device. `capped`: an
// AM above m_maxEager, with m_rdvMax of them in flight.
CommBackendLCI::PostResult CommBackendLCI::postOp(const PendingOp &op) {
  bool rdv = op.kind == PendingOp::AM && op.size > m_maxEager;
  if (rdv && detail::g_rdv_inflight.fetch_add(1, std::memory_order_relaxed) >=
                 m_rdvMax) {
    detail::g_rdv_inflight.fetch_sub(1, std::memory_order_relaxed);
    return PostResult::capped;
  }
  auto args = new localCallbackArgs{op.localComp, op.user_context, rdv};
  void *buf = const_cast<void *>(op.local_buf);
  lci::status_t status;
  switch (op.kind) {
  case PendingOp::AM:
    // the LCI tag carries remoteComp
    status = lci::post_am_x(op.rank, buf, op.size, m_local_comp, m_rcomp)
                 .mr(getDeviceMR(op.mr, op.dev))
                 .device(m_devices[op.dev])
                 .tag(op.remoteComp)
                 .user_context(args)();
    break;
  case PendingOp::GET:
    status = lci::post_get_x(op.rank, buf, op.size, m_local_comp,
                             op.remote_disp, op.rmr)
                 .mr(getDeviceMR(op.mr, op.dev))
                 .device(m_devices[op.dev])
                 .user_context(args)();
    break;
  case PendingOp::PUT:
    status = lci::post_put_x(op.rank, buf, op.size, m_local_comp,
                             op.remote_disp, op.rmr)
                 .mr(getDeviceMR(op.mr, op.dev))
                 .device(m_devices[op.dev])
                 .user_context(args)();
    break;
  }
  if (status.is_posted()) return PostResult::posted;
  delete args;
  if (rdv) detail::g_rdv_inflight.fetch_sub(1, std::memory_order_relaxed);
  if (!status.is_retry()) return PostResult::done;
  return status.get_error().errorcode == lci::errorcode_t::retry_lock
             ? PostResult::locked
             : PostResult::retry;
}

// Hands the operation to LCI, or queues it: at once if its rank has a queue
// (then polling up to m_postTries times while it has), else after m_postTries
// attempts (one inside a completion callback) with progress in between. An attempt that only lost the device's post lock to
// another thread is repeated up to kLockTries times without counting. Never
// waits for LCI to take the operation.
void CommBackendLCI::submit(const PendingOp &op) {
  const int kLockTries = 1000;
  bool mayProgress = !detail::g_in_progress_callback;
  // An AM above m_maxEager polls only between attempts; the others, after
  // every post.
  bool pollAlways = op.kind != PendingOp::AM || op.size <= m_maxEager;
  if (m_nPending.load(std::memory_order_relaxed) != 0 &&
      m_pendingOfRank[op.rank].load(std::memory_order_relaxed) != 0) {
    enqueue(op);
    // Give a destination that is receiving the chance to take what is queued
    // before the caller allocates its next message.
    if (mayProgress)
      for (int i = 0; i < m_postTries &&
                      m_pendingOfRank[op.rank].load(std::memory_order_relaxed) != 0;
           i++)
        progressForPost(op.dev);
    return;
  }
  int tries = 0, lockTries = 0;
  while (true) {
    PostResult r = postOp(op);
    if (mayProgress && (pollAlways || r == PostResult::retry ||
                        r == PostResult::locked))
      progressForPost(op.dev);
    if (r == PostResult::posted) return;
    if (r == PostResult::done) {
      op.localComp({op.local_buf, op.size, op.user_context});
      return;
    }
    if (r == PostResult::capped || !mayProgress) break;
    if (r == PostResult::locked ? ++lockTries >= kLockTries
                                : ++tries >= m_postTries)
      break;
  }
  enqueue(op);
}

void CommBackendLCI::enqueue(const PendingOp &op) {
  std::lock_guard<std::mutex> lock(m_pendingMutex);
  std::deque<PendingOp> &q = m_pending[op.rank];
  if (q.empty()) m_pendingRanks.push_back(op.rank);
  q.push_back(op);
  m_pendingOfRank[op.rank].fetch_add(1, std::memory_order_relaxed);
  m_pendingBytes.fetch_add(op.size, std::memory_order_relaxed);
  m_nPending.fetch_add(1, std::memory_order_relaxed);
}

// Posts queued operations, each rank's oldest first, until LCI refuses; the
// pass ends after kFailedRanks ranks were refused, and the next one starts
// with the ranks this one did not reach. One thread at a time (try-lock), and
// an operation leaves its queue under the lock once posted, so it is posted
// once. Handlers of operations that completed in the post run after the lock
// is released, since they may issue operations.
void CommBackendLCI::postPending() {
  const int kFailedRanks = 4;
  std::unique_lock<std::mutex> lock(m_pendingMutex, std::try_to_lock);
  if (!lock.owns_lock()) return;  // being posted, or appended to
  detail::g_in_post_pending = true;
  std::vector<PendingOp> completed;
  int failed = 0;
  for (size_t n = m_pendingRanks.size(); n > 0 && failed < kFailedRanks; n--) {
    int rank = m_pendingRanks.front();
    m_pendingRanks.pop_front();
    auto it = m_pending.find(rank);
    std::deque<PendingOp> &q = it->second;
    while (!q.empty()) {
      PostResult r = postOp(q.front());
      if (r != PostResult::posted && r != PostResult::done) break;
      if (r == PostResult::done) completed.push_back(q.front());
      m_pendingBytes.fetch_sub(q.front().size, std::memory_order_relaxed);
      q.pop_front();
      m_pendingOfRank[rank].fetch_sub(1, std::memory_order_relaxed);
      m_nPending.fetch_sub(1, std::memory_order_release);
    }
    if (q.empty()) {
      m_pending.erase(it);
    } else {
      m_pendingRanks.push_back(rank);
      failed++;
    }
  }
  lock.unlock();
  for (const PendingOp &op : completed)
    op.localComp({op.local_buf, op.size, op.user_context});
  detail::g_in_post_pending = false;
}

size_t CommBackendLCI::pendingSends(size_t *bytes) {
  if (bytes) *bytes = m_pendingBytes.load(std::memory_order_relaxed);
  return m_nPending.load(std::memory_order_relaxed);
}

// Polls every device and posts from the queues until they are empty. Gives up
// once kFlushStallSeconds pass with no queued operation posted (a destination
// that never drains), and reports what is left.
void CommBackendLCI::flushSends() {
  const double kFlushStallSeconds = 60.0;
  size_t last = m_nPending.load(std::memory_order_acquire);
  double lastChange = CmiWallTimer();
  for (unsigned long iter = 1; last != 0; iter++) {
    for (int i = 0; i < (int)m_devices.size(); i++)
      progressDevice(i);
    postPending();
    size_t now = m_nPending.load(std::memory_order_acquire);
    if (now != last) {
      last = now;
      lastChange = CmiWallTimer();
    } else if (iter % 1024 == 0 &&
               CmiWallTimer() - lastChange > kFlushStallSeconds) {
      std::string ranks;
      int listed = 0;
      {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        for (const auto &entry : m_pending) {
          if (listed++ == 16) {
            ranks += " ...";
            break;
          }
          ranks += " " + std::to_string(entry.first) + "(" +
                   std::to_string(entry.second.size()) + ")";
        }
      }
      CmiPrintf("Reconverse> Warning: process %d gives up flushing its sends "
                "after %.0f s without progress: %zu operations (%zu bytes) "
                "were not handed to the network; destination process "
                "(operations):%s\n",
                lci::get_rank_me(), kFlushStallSeconds, now,
                m_pendingBytes.load(std::memory_order_relaxed), ranks.c_str());
      return;
    }
  }
}

// What getRMR hands out for a registered buffer: the remote memory region on
// ONE device, and that device's index. A one-sided operation has to use the
// same device index on both ends (LCI creates devices collectively, so device
// i of each rank is connected to device i of every other), so the buffer's
// owner names the device and the initiator posts on it, whichever device the
// initiator's own thread uses. Handing out every device's region instead made
// the size grow with +lci_ndevices and outgrow CMK_NOCOPY_DIRECT_BYTES at two
// devices (#203).
struct WireRMR {
  uint32_t device_idx;
  uint32_t reserved;
  lci::rmr_t rmr;
};

static WireRMR readWireRMR(const void *rmr) {
  WireRMR w{};
  if (rmr != nullptr)
    std::memcpy(&w, rmr, sizeof(w));
  return w;
}

void CommBackendLCI::issueRget(int rank, const void *local_buf, size_t size,
                                mr_t local_mr, void* remote_buf, void *rmr,
                                CompHandler localComp, void *user_context) {
  WireRMR remote = readWireRMR(rmr);
  int dev = checkedDeviceIndex(remote.device_idx);
  uintptr_t remote_disp = (uintptr_t)remote_buf - remote.rmr.base;
  submit(PendingOp{PendingOp::GET, rank, dev, local_buf, size, local_mr,
                   localComp, 0, user_context, remote_disp, remote.rmr});
}

void CommBackendLCI::issueRput(int rank, const void *local_buf, size_t size,
                                mr_t local_mr, uintptr_t remote_disp, void *rmr,
                                CompHandler localComp, void *user_context) {
  WireRMR remote = readWireRMR(rmr);
  int dev = checkedDeviceIndex(remote.device_idx);
  submit(PendingOp{PendingOp::PUT, rank, dev, local_buf, size, local_mr,
                   localComp, 0, user_context, remote_disp, remote.rmr});
}

bool CommBackendLCI::threadMustPoll() {
  return detail::g_thread_context.device_poller;
}

bool CommBackendLCI::progressDevice(int dev_idx) {
  // OFI fi_cq_read is not thread-safe. Use a per-device trylock so at most
  // one thread polls each device CQ at a time. Callers that lose the race
  // simply return without making progress — the winning thread covers them.
  bool expected = false;
  if (!m_progress_locks[dev_idx].v.compare_exchange_strong(
          expected, true, std::memory_order_acquire,
          std::memory_order_relaxed)) {
    return false;
  }
  bool prev_in_cb = detail::g_in_progress_callback;
  detail::g_in_progress_callback = true;
  auto ret = lci::progress_x().device(m_devices[dev_idx])();
  detail::g_in_progress_callback = prev_in_cb;
  m_progress_locks[dev_idx].v.store(false, std::memory_order_release);
  return ret.is_done();
}

bool CommBackendLCI::progress(void) {
  bool did_progress = false;
  if (!detail::g_thread_context.tls_device.is_empty()) {
    did_progress = progressDevice(detail::g_thread_context.device_idx);
  } else {
    for (int i = 0; i < (int)m_devices.size(); i++)
      if (progressDevice(i))
        did_progress = true;
  }
  if (m_nPending.load(std::memory_order_relaxed) != 0 &&
      !detail::g_in_progress_callback && !detail::g_in_post_pending)
    postPending();
  return did_progress;
}

// A post retried on another thread's device would otherwise wait for that
// device's poller to drain it.
void CommBackendLCI::progressForPost(int dev_idx) {
  progress();
  if (dev_idx != detail::g_thread_context.device_idx &&
      !detail::g_thread_context.tls_device.is_empty())
    progressDevice(dev_idx);
}

void CommBackendLCI::barrier(void) {
  // nonblocking barrier
  lci::comp_t comp = lci::alloc_counter();
  lci::barrier_x().device(getThreadLocalDevice()).comp(comp)();
  while (lci::counter_get(comp) == 0) {
    progress();
  }
  lci::free_comp(&comp);
}

// Registers nothing yet: getDeviceMR registers on a device when a transfer
// first needs it, and getRMR does for the device it exports.
mr_t CommBackendLCI::registerMemory(void *addr, size_t size) {
  auto region = new LazyMR(addr, size, m_devices.size());
  {
    std::lock_guard<std::mutex> lock(m_liveMrsMutex);
    m_liveMrs.insert(region);
  }
  return region;
}

// The message pool's blocks. Any thread may send from a block, on its own
// device, so the block is registered on every device when it is allocated,
// not on the send path the first time a device sends from it.
mr_t CommBackendLCI::registerMemoryAllDevices(void *addr, size_t size) {
  mr_t mr = registerMemory(addr, size);
  for (int i = 0; i < (int)m_devices.size(); i++)
    getDeviceMR(mr, i);
  return mr;
}

size_t CommBackendLCI::getRMR(mr_t mr, void *addr, size_t size) {
  if (addr && size >= sizeof(WireRMR)) {
    // The owner's own device, so one-sided traffic spreads over the devices
    // the way the buffers' owners do. A thread that is not a PE uses device 0.
    int dev = detail::g_thread_context.device_idx;
    if (dev < 0)
      dev = 0;
    WireRMR w{};
    w.device_idx = dev;
    w.rmr = lci::get_rmr(getDeviceMR(mr, dev));
    std::memcpy(addr, &w, sizeof(w));
  }
  return sizeof(WireRMR);
}

void CommBackendLCI::deregisterMemory(mr_t mr) {
  {
    std::lock_guard<std::mutex> lock(m_liveMrsMutex);
    m_liveMrs.erase(mr);
  }
  releaseRegion(mr);
}

// Deregisters the devices a region was registered on, and frees it.
void CommBackendLCI::releaseRegion(mr_t mr) {
  auto region = static_cast<LazyMR *>(mr);
  for (int i = 0; i < (int)m_devices.size(); i++) {
    lci::mr_impl_t *impl = region->slots[i].load(std::memory_order_acquire);
    if (impl != nullptr) {
      lci::mr_t m(impl);
      lci::deregister_memory(&m);
    }
  }
  delete region;
}

// Release any memory regions the application never handed back, e.g. buffers
// registered with CK_BUFFER_NODEREG. Must run while the devices are still alive
// (releaseRegion walks m_devices), so exit() calls this before free_device.
void CommBackendLCI::deregisterAllMemory() {
  std::unordered_set<void *> leftovers;
  {
    std::lock_guard<std::mutex> lock(m_liveMrsMutex);
    leftovers.swap(m_liveMrs);
  }
  for (void *mr : leftovers)
    releaseRegion(mr);
}

lci::device_t CommBackendLCI::getThreadLocalDevice() {
  if (detail::g_thread_context.tls_device.is_empty()) {
    CmiAssert(m_devices.size() > 0);
    return m_devices[0];
  } else {
    return detail::g_thread_context.tls_device;
  }
}

// The registration for the device getThreadLocalDevice() posts on: device 0
// for a thread without one, whose device_idx is -1 after exitThread().
lci::mr_t CommBackendLCI::getThreadLocalMR(mr_t mr) {
  int dev = detail::g_thread_context.tls_device.is_empty()
                ? 0
                : detail::g_thread_context.device_idx;
  return getDeviceMR(mr, dev);
}

// Registers the region on the device the first time it is asked for. Two
// threads can ask at once, e.g. the initiators of two gets from one buffer:
// both register, the first to publish wins, and the other releases its own.
lci::mr_t CommBackendLCI::getDeviceMR(mr_t mr, int device_idx) {
  if (mr == MR_NULL) {
    return lci::mr_t();
  }
  auto region = static_cast<LazyMR *>(mr);
  std::atomic<lci::mr_impl_t *> &slot = region->slots[device_idx];
  lci::mr_impl_t *impl = slot.load(std::memory_order_acquire);
  if (impl == nullptr) {
    lci::mr_impl_t *mine = lci::register_memory_x(region->addr, region->size)
                               .device(m_devices[device_idx])()
                               .get_impl();
    if (slot.compare_exchange_strong(impl, mine, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      impl = mine;
    } else {
      lci::mr_t lost(mine);
      lci::deregister_memory(&lost);
    }
  }
  return lci::mr_t(impl);
}

// LCI registers on the fly, and deregisters after delivery, every rendezvous
// AM receive buffer it gets without a registration. With many large AMs in
// flight those registrations ran the NIC out of them: cxi fi_mr_regattr "No
// space left on device" at 2 processes x 64 PEs (uiuc-hpc/lci#225). CmiAlloc
// takes the buffer from the polling thread's message pool, whose blocks
// alloc_mempool_block registered on every device, so LCI gets that instead.
#if RECONVERSE_LCI_ALLOCATE_WITH_MR
void *AllocatorLCI::allocate_with_mr(size_t size, lci::device_t device,
                                     lci::mr_t *mr) {
  void *msg = CmiAlloc(size);
  *mr = g_lci_backend ? g_lci_backend->getMRForDevice(MRFIELD(msg), device)
                      : lci::mr_t();
  return msg;
}
#endif

// A buffer from outside the pool (BIG_MSG and up) is registered here on that
// one device, and deregistered when it is freed. Empty, so LCI registers the
// buffer itself, for a buffer with no region, a device that is not ours, or
// once exit() has released the regions: free_device still drains completions,
// and the pool blocks' handles are stale by then.
lci::mr_t CommBackendLCI::getMRForDevice(mr_t mr, lci::device_t device) {
  if (mr == MR_NULL || g_pool_mrs_released)
    return lci::mr_t();
  for (int i = 0; i < (int)m_devices.size(); i++)
    if (m_devices[i] == device)
      return getDeviceMR(mr, i);
  return lci::mr_t();
}

// The device a remote buffer's owner named. Every process must run with the
// same +lci_ndevices; checked in every build, since an index past the end
// would post on a device that does not exist.
int CommBackendLCI::checkedDeviceIndex(uint32_t device_idx) {
  if (device_idx >= m_devices.size())
    CmiAbort("LCI backend: a remote buffer is registered on device %u, but "
             "this process has %zu devices; run every process with the same "
             "+lci_ndevices\n", device_idx, m_devices.size());
  return (int)device_idx;
}

} // namespace lci_impl
} // namespace comm_backend
