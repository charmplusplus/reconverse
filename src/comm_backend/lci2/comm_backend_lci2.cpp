#include <time.h>
#include "mempool.h"
#include "lci.hpp"
#include "comm_backend/lci2/comm_backend_lci2.h"
#include <vector>
namespace comm_backend { extern SignalHandler g_signal_handler; }   // comm_backend_internal.cpp

// Where an active-message send spends its time, per thread (printed at
// exitThread when anything was sent): the LCI post, the progress call that
// follows it, retries, and how often the post completed inline.
namespace {
struct AmSendStats {
  long n = 0, retries = 0, inline_done = 0;
  double post_s = 0.0, progress_s = 0.0;
  long retry_lock = 0, retry_nopacket = 0, retry_nomem = 0, retry_backlog = 0, retry_other = 0;
  double max_post_s = 0.0;   // the longest single post (its retries included)
};
thread_local AmSendStats g_am_send_stats;
static inline double amNow() {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}
}  // namespace

CpvDeclare(mempool_type*, mempool);

namespace comm_backend {
namespace lci2_impl {

namespace detail {
struct ThreadContext {
  int thread_id;
  int device_idx;
  lci::device_t tls_device;
};

thread_local ThreadContext g_thread_context;

// Set while a thread is inside lci::progress_x() (i.e., executing a completion
// callback fired by progress). Used to break the reentrancy deadlock: if issueAm
// or issueRget are called from within a callback, they must not spin-retry (which
// would hold m_progress_locks indefinitely and prevent CQ polling). Instead they
// push to the LCI backlog queue with allow_retry=false and return immediately.
thread_local bool g_in_progress_callback = false;
} // namespace detail

std::vector<CompHandler> g_handlers;

struct localCallbackArgs {
  CompHandler handler;
  void *user_context;
};

void localCallback(lci::status_t status) {
  auto args = reinterpret_cast<localCallbackArgs *>(status.get_user_context());
  auto handler = args->handler;
  auto user_context = args->user_context;
  delete args;
  handler({status.get_buffer(), status.get_size(), user_context});
}

void remoteCallback(lci::status_t status) {
  auto am_handler = static_cast<AmHandler>(status.get_tag());
  auto handler = g_handlers[am_handler];
  handler({status.get_buffer(), status.get_size(), nullptr});
}

// A put with signal landed here: rank, the 16-bit tag and the bytes written.
void signalCallback(lci::status_t status) {
  if (comm_backend::g_signal_handler) comm_backend::g_signal_handler(status.rank, (uint32_t)status.get_tag(), status.get_size());
}

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
    *mem_hndl = registerMemory(pool, *size);
    return pool;
}

void free_mempool_block(void *ptr, void* mem_hndl)
{
    // Deregister the MR before releasing the host pages it covers.
    if (mem_hndl != NULL)
        deregisterMemory((mr_t) mem_hndl);
    // Use ::free explicitly: unqualified `free` here resolves to comm_backend::free,
    // which expects a CmiChunkHeader-prefixed user pointer. This function receives
    // a raw posix_memalign block, so we need the C standard library free.
    ::free(ptr);
}

void CommBackendLCI2::init_mempool()
{
  CpvInitialize(mempool_type*, mempool);

  CpvAccess(mempool) = mempool_init(mempool_options.mempool_init_size,
    alloc_mempool_block,
    free_mempool_block,
    mempool_options.mempool_max_size);
}

void* CommBackendLCI2::malloc(int n_bytes, int header)
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

void CommBackendLCI2::free(void *msg)
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
    // For in-pool messages, free from mempool_header position
    mempool_free_thread(aligned_addr);
  }
}

void CommBackendLCI2::init(char **argv) {
  int num_devices = 1;
  CmiGetArgInt(argv, "+lci_ndevices", (int *)&num_devices);
  CmiAssert(num_devices >= 1);

  lci::global_initialize();
  auto g_attr = lci::get_g_default_attr();
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
  m_progress_locks = std::vector<std::atomic<bool>>(num_devices);
  for (int i = 0; i < num_devices; i++) {
    m_devices[i] = lci::alloc_device();
    m_progress_locks[i].store(false, std::memory_order_relaxed);
  }

  m_local_comp = lci::alloc_handler(localCallback);
  m_remote_comp = lci::alloc_handler(remoteCallback);
  m_rcomp = lci::register_rcomp(m_remote_comp);
  m_signal_comp = lci::alloc_handler(signalCallback);
  m_signal_rcomp = lci::register_rcomp(m_signal_comp);
  if (lci::get_rank_me() == 0) fprintf(stderr, "[lci2] am rcomp=%u signal rcomp=%u max_imm_tag=%llu max_imm_rcomp=%llu\n", (unsigned)m_rcomp, (unsigned)m_signal_rcomp, (unsigned long long)lci::get_g_runtime().get_attr_max_imm_tag(), (unsigned long long)lci::get_g_runtime().get_attr_max_imm_rcomp());
  lci::set_allocator(&m_allocator);
  barrier();
}

void CommBackendLCI2::exit() {
  // Drop any still-registered memory regions before tearing the devices down;
  // an open region makes fi_close(domain) fail with FI_EBUSY. This supersedes
  // the reconverse_lci_cache_amd workaround of skipping free_device entirely.
  deregisterAllMemory();
  for (auto device : m_devices) {
    lci::free_device(&device);
  }
  m_devices.clear();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
  lci::free_comp(&m_signal_comp);
  lci::g_runtime_fina();
  lci::global_finalize();
}

void CommBackendLCI2::initThread(int thread_id, int num_threads) {
  detail::g_thread_context.thread_id = thread_id;
  int nthreads_per_device =
      (num_threads + m_devices.size() - 1) / m_devices.size();
  int device_id = thread_id / nthreads_per_device;
  CmiAssert(device_id < m_devices.size());
  detail::g_thread_context.device_idx = device_id;
  detail::g_thread_context.tls_device = m_devices[device_id];
}

void CommBackendLCI2::exitThread() {
  if (g_am_send_stats.n > 0)
    fprintf(stderr, "[am-send] rank %d thread %d: n=%ld post mean_us=%.2f progress-after-post mean_us=%.2f retries=%ld inline-done=%ld "
            "| retry reasons: lock=%ld nopacket=%ld nomem=%ld backlog=%ld other=%ld | longest post %.1f us\n",
            lci::get_rank_me(), detail::g_thread_context.thread_id, g_am_send_stats.n,
            g_am_send_stats.post_s / g_am_send_stats.n * 1e6,
            g_am_send_stats.progress_s / g_am_send_stats.n * 1e6, g_am_send_stats.retries,
            g_am_send_stats.inline_done, g_am_send_stats.retry_lock, g_am_send_stats.retry_nopacket,
            g_am_send_stats.retry_nomem, g_am_send_stats.retry_backlog, g_am_send_stats.retry_other,
            g_am_send_stats.max_post_s * 1e6);
  // Destroy this PE's mempool while the LCI devices and runtime are still alive,
  // so its registered memory blocks are deregistered (deregisterMemory uses
  // m_devices, and the devices/runtime are only torn down later in exit() on
  // rank 0). The mempool is a per-PE Cpv, so each thread destroys its own.
  if (CpvInitialized(mempool) && CpvAccess(mempool) != NULL) {
    mempool_destroy(CpvAccess(mempool));
    CpvAccess(mempool) = NULL;
  }
  detail::g_thread_context.tls_device = lci::device_t();
  detail::g_thread_context.device_idx = -1;
  detail::g_thread_context.thread_id = -1;
}

int CommBackendLCI2::getMyNodeId() { return lci::get_rank_me(); }

int CommBackendLCI2::getNumNodes() { return lci::get_rank_n(); }

AmHandler CommBackendLCI2::registerAmHandler(CompHandler handler) {
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}


void CommBackendLCI2::issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
                              CompHandler localComp, AmHandler remoteComp, void *user_context) {
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
  // When called from inside a completion callback (g_in_progress_callback=true),
  // m_progress_locks is held by the outer progress() frame. Spinning here would
  // hold that lock indefinitely, preventing CQ polling and deadlocking the TX
  // queue. Use allow_retry=false so LCI pushes to its backlog queue and returns
  // immediately; the backlog is drained in the next progress() call.
  bool allow_retry = !detail::g_in_progress_callback;
  AmSendStats &st = g_am_send_stats;
  st.n++;
  const double tpost0 = amNow();
  do {
    // we use LCI tag to pass the remoteComp
    const double t0 = amNow();
    status = lci::post_am_x(rank, const_cast<void *>(local_buf), size,
                            m_local_comp, m_rcomp)
                 .mr(getThreadLocalMR(mr))
                 .device(getThreadLocalDevice())
                 .tag(remoteComp)
                 .user_context(args)
                 .allow_retry(allow_retry)();
    const double t1 = amNow();
    st.post_s += t1 - t0;
    if (allow_retry) progress();
    st.progress_s += amNow() - t1;
    if (status.is_retry()) {
      st.retries++;
      switch (status.error.errorcode) {
        case lci::errorcode_t::retry_lock: st.retry_lock++; break;
        case lci::errorcode_t::retry_nopacket: st.retry_nopacket++; break;
        case lci::errorcode_t::retry_nomem: st.retry_nomem++; break;
        case lci::errorcode_t::retry_backlog: st.retry_backlog++; break;
        default: st.retry_other++; break;
      }
    }
  } while (status.is_retry());
  { const double dt = amNow() - tpost0; if (dt > st.max_post_s) st.max_post_s = dt; }
  if (status.is_done()) {
    st.inline_done++;
    localComp({local_buf, size, user_context});
    delete args;
  }
}

void CommBackendLCI2::issueRget(int rank, const void *local_buf, size_t size,
                                mr_t local_mr, void* remote_buf, void *rmr,
                                CompHandler localComp, void *user_context) {
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
  uintptr_t remote_disp = (uintptr_t)remote_buf - getThreadLocalRMR(rmr).base;
  // Same reentrancy guard as issueAm: don't spin when called from a completion
  // callback, push to backlog instead.
  bool allow_retry = !detail::g_in_progress_callback;
  do {
    status = lci::post_get_x(rank, const_cast<void *>(local_buf), size,
                             m_local_comp, remote_disp, getThreadLocalRMR(rmr))
                 .mr(getThreadLocalMR(local_mr))
                 .device(getThreadLocalDevice())
                 .user_context(args)
                 .allow_retry(allow_retry)();
    if (allow_retry) progress();
  } while (status.is_retry());
  if (status.is_done()) {
    localComp({local_buf, size, user_context});
    delete args;
  }
}

void CommBackendLCI2::issueRput(int rank, const void *local_buf, size_t size,
                                mr_t local_mr, void *remote_buf, void *rmr,
                                CompHandler localComp, void *user_context) {
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
  // The remote address has to be turned into a displacement from the region
  // base, exactly as issueRget does. The base is not the address the peer asked
  // to register: with the registration cache on, LCI records the enclosing
  // rcache region's page-aligned start (see mem_reg_cb in lci reg_cache.cpp),
  // so a hardcoded displacement of 0 writes ahead of the intended buffer by
  // however far into its region the buffer sits.
  uintptr_t remote_disp = (uintptr_t)remote_buf - getThreadLocalRMR(rmr).base;
  do {
    status = lci::post_put_x(rank, const_cast<void *>(local_buf), size,
                             m_local_comp, remote_disp, getThreadLocalRMR(rmr))
                 .mr(getThreadLocalMR(local_mr))
                 .device(getThreadLocalDevice())
                 .user_context(args)();
    progress();
  } while (status.is_retry());
  if (status.is_done()) {
    localComp({local_buf, size, user_context});
    delete args;
  }
}

void CommBackendLCI2::issueRputSignal(int rank, const void *local_buf, size_t size,
                                      mr_t local_mr, void *remote_buf, void *rmr, uint32_t tag,
                                      CompHandler localComp, void *user_context) {
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
  uintptr_t remote_disp = (uintptr_t)remote_buf - getThreadLocalRMR(rmr).base;
  do {
    status = lci::post_put_x(rank, const_cast<void *>(local_buf), size,
                             m_local_comp, remote_disp, getThreadLocalRMR(rmr))
                 .mr(getThreadLocalMR(local_mr))
                 .device(getThreadLocalDevice())
                 .tag(tag)
                 .remote_comp(m_signal_rcomp)
                 .user_context(args)();
    progress();
  } while (status.is_retry());
  if (status.is_done()) {
    localComp({local_buf, size, user_context});
    delete args;
  }
}

bool CommBackendLCI2::progress(void) {
  // OFI fi_cq_read is not thread-safe. Use a per-device trylock so at most
  // one thread polls each device CQ at a time. Callers that lose the race
  // simply return without making progress — the winning thread covers them.
  if (!detail::g_thread_context.tls_device.is_empty()) {
    int dev_idx = detail::g_thread_context.device_idx;
    bool expected = false;
    if (!m_progress_locks[dev_idx].compare_exchange_strong(
            expected, true, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      return false;
    }
    // Mark that completion callbacks fired inside progress_x() are reentrant.
    // issueAm/issueRget check this flag to avoid spinning (which would hold
    // m_progress_locks indefinitely and deadlock the TX queue drain path).
    detail::g_in_progress_callback = true;
    auto ret = lci::progress_x().device(detail::g_thread_context.tls_device)();
    detail::g_in_progress_callback = false;
    m_progress_locks[dev_idx].store(false, std::memory_order_release);
    return ret.is_done();
  } else {
    bool did_progress = false;
    for (int i = 0; i < (int)m_devices.size(); i++) {
      bool expected = false;
      if (!m_progress_locks[i].compare_exchange_strong(
              expected, true, std::memory_order_acquire,
              std::memory_order_relaxed)) {
        continue;
      }
      detail::g_in_progress_callback = true;
      auto ret = lci::progress_x().device(m_devices[i])();
      detail::g_in_progress_callback = false;
      m_progress_locks[i].store(false, std::memory_order_release);
      if (ret.is_done())
        did_progress = true;
    }
    return did_progress;
  }
}

void CommBackendLCI2::barrier(void) {
  // nonblocking barrier
  lci::comp_t comp = lci::alloc_counter();
  lci::barrier_x().device(getThreadLocalDevice()).comp(comp)();
  while (lci::counter_get(comp) == 0) {
    progress();
  }
  lci::free_comp(&comp);
}

mr_t CommBackendLCI2::registerMemory(void *addr, size_t size) {
  auto mrs = new lci::mr_impl_t *[m_devices.size()];
  for (int i = 0; i < m_devices.size(); i++) {
    mrs[i] =
        lci::register_memory_x(addr, size).device(m_devices[i])().get_impl();
  }
  {
    std::lock_guard<std::mutex> lock(m_liveMrsMutex);
    m_liveMrs.insert(mrs);
  }
  return mrs;
}

size_t CommBackendLCI2::getRMR(mr_t mr, void *addr, size_t size) {
  lci::mr_t *mrs = (lci::mr_t *)mr;
  if (addr && size >= sizeof(lci::rmr_t) * m_devices.size()) {
    for (int i = 0; i < m_devices.size(); i++) {
      lci::rmr_t rkey = lci::get_rmr(lci::mr_t(mrs[i]));
      std::memcpy(addr, &rkey, sizeof(lci::rmr_t));
      addr = (char *)addr + sizeof(lci::rmr_t);
    }
  }
  return sizeof(lci::rmr_t) * m_devices.size();
}

void CommBackendLCI2::deregisterMemory(mr_t mr_) {
  {
    std::lock_guard<std::mutex> lock(m_liveMrsMutex);
    m_liveMrs.erase(mr_);
  }
  lci::mr_t *mrs = (lci::mr_t *)mr_;
  for (int i = 0; i < m_devices.size(); i++) {
    lci::mr_t mr(mrs[i]);
    lci::deregister_memory(&mr);
  }
  delete[] mrs;
}

// Release any memory regions the application never handed back, e.g. buffers
// registered with CK_BUFFER_NODEREG. Must run while the devices are still alive
// (deregisterMemory walks m_devices), so exit() calls this before free_device.
void CommBackendLCI2::deregisterAllMemory() {
  std::unordered_set<void *> leftovers;
  {
    std::lock_guard<std::mutex> lock(m_liveMrsMutex);
    leftovers.swap(m_liveMrs);
  }
  // Deregister outside the lock: deregisterMemory() takes it too.
  for (void *mr : leftovers) {
    lci::mr_t *mrs = (lci::mr_t *)mr;
    for (int i = 0; i < m_devices.size(); i++) {
      lci::mr_t m(mrs[i]);
      lci::deregister_memory(&m);
    }
    delete[] mrs;
  }
}

lci::device_t CommBackendLCI2::getThreadLocalDevice() {
  if (detail::g_thread_context.tls_device.is_empty()) {
    CmiAssert(m_devices.size() > 0);
    return m_devices[0];
  } else {
    return detail::g_thread_context.tls_device;
  }
}

lci::mr_t CommBackendLCI2::getThreadLocalMR(mr_t mr) {
  if (mr == MR_NULL) {
    return lci::mr_t();
  }
  return lci::mr_t(
      static_cast<lci::mr_impl_t **>(mr)[detail::g_thread_context.device_idx]);
}

lci::rmr_t CommBackendLCI2::getThreadLocalRMR(void *rmr) {
  if (rmr == nullptr) {
    return lci::rmr_t();
  }
  return static_cast<lci::rmr_t *>(rmr)[detail::g_thread_context.device_idx];
}

} // namespace lci2_impl
} // namespace comm_backend
