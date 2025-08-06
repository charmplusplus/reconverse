#include "converse_internal.h"

namespace comm_backend {

namespace detail {
struct ThreadContext {
  int thread_id;
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

void CommBackendLCI2::init(char **argv) {
  lci::g_runtime_init();

  int num_tls_devices = 0;
  CmiGetArgInt(argv, "+lci_ndevices", (int *)&num_tls_devices);
  m_devices.resize(num_tls_devices);
  for (int i = 0; i < num_tls_devices; i++) {
    m_devices[i] = lci::alloc_device();
  }

  m_local_comp = lci::alloc_handler(localCallback);
  m_remote_comp = lci::alloc_handler(remoteCallback);
  m_rcomp = lci::register_rcomp(m_remote_comp);
  lci::set_allocator(&m_allocator);
  lci::barrier();
}

void CommBackendLCI2::exit() {
  for (auto device : m_devices) {
    lci::free_device(&device);
  }
  m_devices.clear();
  lci::g_runtime_fina();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
}

void CommBackendLCI2::initThread(int thread_id, int num_threads) {
  if (m_devices.empty()) {
    detail::g_thread_context.tls_device = lci::device_t();
    return;
  }
  detail::g_thread_context.thread_id = thread_id;
  int nthreads_per_device =
      (num_threads + m_devices.size() - 1) / m_devices.size();
  int device_id = thread_id / nthreads_per_device;
  CmiAssert(device_id < m_devices.size());
  detail::g_thread_context.tls_device = m_devices[device_id];
}

void CommBackendLCI2::exitThread() {
  detail::g_thread_context.tls_device = lci::device_t();
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
  auto post_am = lci::post_am_x(rank, const_cast<void *>(local_buf), size,
                                m_local_comp, m_rcomp)
                     .mr(mr)
                     .tag(remoteComp)
                     .user_context(args);
  if (mr == MR_NULL && !detail::g_thread_context.tls_device.is_empty()) {
    // For now, we only use the thread-local device for non-registered memory.
    post_am.device(detail::g_thread_context.tls_device);
  }
  lci::status_t status;
  do {
    // we use LCI tag to pass the remoteComp
    status = post_am();
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
    status = lci::post_get_x(rank, const_cast<void *>(local_buf), size, m_local_comp, remote_disp,
                             *static_cast<lci::rmr_t *>(rmr))
                 .mr(local_mr)
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
    status = lci::post_put_x(rank, const_cast<void *>(local_buf), size, m_local_comp, remote_disp,
                             *static_cast<lci::rmr_t *>(rmr))
                 .mr(local_mr)
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
  } else if (!m_devices.empty()) {
    // There are thread-local devices but we are not assigned one.
    // We are the main thread. Make progress on all devices.
    for (auto &device : m_devices) {
      auto ret = lci::progress_x().device(device)();
      if (ret.is_done())
        return true;
    }
  }
  // Then make progress on the global default device
  auto ret = lci::progress();
  return ret.is_done();
}

void CommBackendLCI2::barrier(void) {
  // nonblocking barrier
  lci::comp_t comp = lci::alloc_sync();
  lci::barrier_x().comp(comp)();
  while (!lci::sync_test(comp, nullptr)) {
    progress();
  }
  lci::free_comp(&comp);
}

mr_t CommBackendLCI2::registerMemory(void *addr, size_t size) {
  auto mr = lci::register_memory(addr, size);
  return mr.get_impl();
}

size_t CommBackendLCI2::getRMR(mr_t mr, void *addr, size_t size) {
  if (addr && size >= sizeof(lci::rmr_t)) {
    lci::rmr_t rkey = lci::get_rmr(mr);
    std::memcpy(addr, &rkey, sizeof(lci::rmr_t));
  }
  return sizeof(lci::rmr_t);
}

void CommBackendLCI2::deregisterMemory(mr_t mr_) {
  lci::mr_t mr(mr_);
  lci::deregister_memory(&mr);
}

} // namespace comm_backend
