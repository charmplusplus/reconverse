#include "converse_internal.h"

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

void CommBackendLCI2::init(char **argv) {
  lci::g_runtime_init_x().alloc_default_device(false)();

  int num_devices = 1;
  CmiGetArgInt(argv, "+lci_ndevices", (int *)&num_devices);
  CmiAssert(num_devices >= 1);
  auto g_attr = lci::get_g_default_attr();
  g_attr.npackets = num_devices * g_attr.npackets;
  lci::set_g_default_attr(g_attr);
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
