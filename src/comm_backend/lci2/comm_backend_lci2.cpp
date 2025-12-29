#include "mempool.h"
#include "lci.hpp"
#include "comm_backend/lci2/comm_backend_lci2.h"
#include <vector>

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

    void *pool;
    posix_memalign(&pool,ALIGNBUF,*size);
    printf("LCI2: Allocated mempool block of size %zu, %zu at %p\n", *size, ALIGNBUF, pool);
    registerMemory(pool, *size);
    return pool;
}

void free_mempool_block(void *ptr, void* mem_hndl)
{
    free(ptr);
    deregisterMemory((mr_t) mem_hndl);
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
        if (res) ptr = res + sizeof(mempool_header);

        size_t offset1=GetMemOffsetFromBase(res);
        mr_t* extractedmr  = (mr_t *) GetMemHndl(res);
      }
    else
      {
        CmiPrintf("Allocating out of pool\n");
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
  uint size = SIZEFIELD((char*)msg);
  printf("LCI2: Freeing message %p of size %u\n", msg, size);
  if (size <= mempool_options.mempool_lb_size)
    CmiAbort("LCI: mempool lower boundary violation");
  else
    size = ALIGN64(size);
  if(size>=BIG_MSG)
  {
    // For out-of-pool messages, go back to out_of_pool_header
    // Layout: [out_of_pool_header][CmiChunkHeader][user data (msg)]
    deregisterMemory( (mr_t)GetMemHndl( (char* )msg ));
    free((char *)msg - headersize - sizeof(out_of_pool_header));
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
  lci::set_g_default_attr(g_attr);

  lci::g_runtime_init_x().alloc_default_device(false)();
  m_devices.resize(num_devices);
  for (int i = 0; i < num_devices; i++) {
    m_devices[i] = lci::alloc_device();
  }

  m_local_comp = lci::alloc_handler(localCallback);
  m_remote_comp = lci::alloc_handler(remoteCallback);
  m_rcomp = lci::register_rcomp(m_remote_comp);
  lci::set_allocator(&m_allocator);
  barrier();
}

void CommBackendLCI2::exit() {
  for (auto device : m_devices) {
    lci::free_device(&device);
  }
  m_devices.clear();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
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
  do {
    // we use LCI tag to pass the remoteComp
    status = lci::post_am_x(rank, const_cast<void *>(local_buf), size,
                            m_local_comp, m_rcomp)
                 .mr(getThreadLocalMR(mr))
                 .device(getThreadLocalDevice())
                 .tag(remoteComp)
                 .user_context(args)();
    progress();
  } while (status.is_retry());
  if (status.is_done()) {
    localComp({local_buf, size, user_context});
    delete args;
  }
}

void CommBackendLCI2::issueRget(int rank, const void *local_buf, size_t size,
                                mr_t local_mr, uintptr_t remote_disp, void *rmr,
                                CompHandler localComp, void *user_context) {
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
  do {
    status = lci::post_get_x(rank, const_cast<void *>(local_buf), size,
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

void CommBackendLCI2::issueRput(int rank, const void *local_buf, size_t size,
                                mr_t local_mr, uintptr_t remote_disp, void *rmr,
                                CompHandler localComp, void *user_context) {
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
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

bool CommBackendLCI2::progress(void) {
  if (!detail::g_thread_context.tls_device.is_empty()) {
    // If we are assigned a thread-local device, make progress on it.
    auto ret = lci::progress_x().device(detail::g_thread_context.tls_device)();
    if (ret.is_done())
      return true;
    else
      return false;
  } else {
    // We are not assigned a thread-local device.
    // We are the main thread. Make progress on all devices.
    bool did_progress = false;
    for (auto &device : m_devices) {
      auto ret = lci::progress_x().device(device)();
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
  lci::mr_t *mrs = (lci::mr_t *)mr_;
  for (int i = 0; i < m_devices.size(); i++) {
    lci::mr_t mr(mrs[i]);
    lci::deregister_memory(&mr);
  }
  delete[] mrs;
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
