#include "converse_internal.h"

namespace comm_backend {
namespace lcw_impl {

CommBackendLCW *g_lcw_backend = nullptr;

namespace detail {
struct ThreadContext {
  int thread_id;
  int device_idx;
  device_t *tls_device;
};

thread_local ThreadContext g_thread_context;

struct RdvHeaderMsg {
  lcw::tag_t rdv_tag;
  size_t size;
};
} // namespace detail

std::vector<CompHandler> g_handlers;

struct localCallbackArgs {
  CompHandler handler;
  void *user_context;
};

void localCallback(lcw::request_t request) {
  auto args = reinterpret_cast<localCallbackArgs *>(request.user_context);
  if (!args) {
    // This is a rdv header message, just free the buffer
    auto rndv_msg = reinterpret_cast<detail::RdvHeaderMsg *>(request.buffer);
    delete rndv_msg;
    return;
  }
  auto handler = args->handler;
  auto user_context = args->user_context;
  delete args;
  handler({request.buffer, static_cast<size_t>(request.length), user_context});
}

void remoteRdvHeaderCallback(lcw::request_t request);

void remoteCallback(lcw::request_t request) {
  size_t msg_length = static_cast<size_t>(request.length);
  if (msg_length == sizeof(detail::RdvHeaderMsg)) {
    // Here we exploit the fact that a normal reconverse message is always
    // larger than sizeof(RdvHeaderMsg).
    remoteRdvHeaderCallback(request);
    return;
  }
  auto am_handler = 0; // only one AM handler is supported
  auto handler = g_handlers[am_handler];
  handler({request.buffer, msg_length, nullptr});
}

void localRdvHeaderCallback(lcw::request_t request) {
  auto rndv_msg = reinterpret_cast<detail::RdvHeaderMsg *>(request.buffer);
  delete rndv_msg;
}

void remoteRdvHeaderCallback(lcw::request_t request) {
  auto header = reinterpret_cast<detail::RdvHeaderMsg *>(request.buffer);
  // post another receive to get the actual AM message
  void *buffer = CmiAlloc(header->size);
  while (!lcw::recv(g_lcw_backend->getThreadLocalDevice()->device, request.rank,
                    header->rdv_tag, buffer, header->size,
                    g_lcw_backend->m_remote_comp, nullptr)) {
    g_lcw_backend->progress();
  }
  CmiFree(request.buffer);
}

void CommBackendLCW::init(char **argv) {
  CmiAssertMsg(g_lcw_backend == nullptr,
               "LCW backend has already been initialized");
  g_lcw_backend = this;

  eager_threshold = 8192;
  CmiGetArgInt(argv, "+lcw_eager_threshold", (int *)&eager_threshold);
  char *backend_str = nullptr;
  CmiGetArgStringDesc(
      argv, "+lcw_backend", &backend_str,
      "LCW backend to use {auto, lci, mpi, lci2, gex}. Default=mpi");
  lcw::backend_t backend = lcw::backend_t::MPI;
  if (backend_str) {
    if (strcmp(backend_str, "auto") == 0) {
      backend = lcw::backend_t::AUTO;
    } else if (strcmp(backend_str, "lci1") == 0) {
      backend = lcw::backend_t::LCI;
    } else if (strcmp(backend_str, "mpi") == 0) {
      backend = lcw::backend_t::MPI;
    } else if (strcmp(backend_str, "lci") == 0) {
      backend = lcw::backend_t::LCI2;
    } else if (strcmp(backend_str, "lci2") == 0) {
      backend = lcw::backend_t::LCI2;
    } else if (strcmp(backend_str, "gex") == 0) {
      backend = lcw::backend_t::GEX;
    }
  }
  lcw::initialize(backend);

  m_local_comp = lcw::alloc_handler(localCallback);
  m_remote_comp = lcw::alloc_handler(remoteCallback);

  int num_devices = 1;
  CmiGetArgInt(argv, "+lcw_ndevices", (int *)&num_devices);
  CmiAssert(num_devices >= 1);
  m_devices = std::vector<device_t>(num_devices);
  for (int i = 0; i < num_devices; i++) {
    m_devices[i].device = lcw::alloc_device(eager_threshold, m_remote_comp);
    m_devices[i].max_rdv_tag = lcw::get_max_tag(m_devices[i].device);
  }

  lcw::set_custom_allocator(&m_allocator);
  barrier();
}

void CommBackendLCW::exit() {
  for (auto &device : m_devices) {
    lcw::free_device(device.device);
  }
  m_devices.clear();
  lcw::free_handler(m_local_comp);
  lcw::free_handler(m_remote_comp);
  lcw::finalize();

  g_lcw_backend = nullptr;
}

void CommBackendLCW::initThread(int thread_id, int num_threads) {
  detail::g_thread_context.thread_id = thread_id;
  int nthreads_per_device =
      (num_threads + m_devices.size() - 1) / m_devices.size();
  int device_id = thread_id / nthreads_per_device;
  CmiAssert(device_id < m_devices.size());
  detail::g_thread_context.device_idx = device_id;
  detail::g_thread_context.tls_device = &m_devices[device_id];
}

void CommBackendLCW::exitThread() {
  detail::g_thread_context.tls_device = nullptr;
  detail::g_thread_context.device_idx = -1;
  detail::g_thread_context.thread_id = -1;
}

int CommBackendLCW::getMyNodeId() { return lcw::get_rank_me(); }

int CommBackendLCW::getNumNodes() { return lcw::get_rank_n(); }

AmHandler CommBackendLCW::registerAmHandler(CompHandler handler) {
  CmiAssertMsg(g_handlers.empty(),
               "Only one AM handler is supported in the LCW backend");
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

void CommBackendLCW::issueAm(int rank, const void *local_buf, size_t size,
                             mr_t mr, CompHandler localComp,
                             AmHandler remoteComp, void *user_context) {
  CmiAssertMsg(remoteComp == 0, "Unexpected remote completion handler");
  if (size <= eager_threshold) {
    auto args = new localCallbackArgs{localComp, user_context};
    while (!lcw::put(getThreadLocalDevice()->device, rank,
                     const_cast<void *>(local_buf), size, m_local_comp, args)) {
      progress();
    }
  } else {
    // Rendezvous protocol
    lcw::tag_t rdv_tag = getThreadLocalDevice()->getNextRdvTag();
    // Send a small control message to the remote side to notify the incoming
    // large AM message.
    detail::RdvHeaderMsg *rndv_msg = new detail::RdvHeaderMsg{rdv_tag, size};
    while (!lcw::put(getThreadLocalDevice()->device, rank, rndv_msg,
                     sizeof(detail::RdvHeaderMsg), m_local_comp, nullptr)) {
      progress();
    }
    // Post a send for actual data
    auto args = new localCallbackArgs{localComp, user_context};
    while (!lcw::send(getThreadLocalDevice()->device, rank, rdv_tag,
                      const_cast<void *>(local_buf), size, m_local_comp,
                      args)) {
      progress();
    }
  }
}

bool CommBackendLCW::progress(void) {
  if (detail::g_thread_context.tls_device) {
    // If we are assigned a thread-local device, make progress on it.
    bool did_something =
        lcw::do_progress(detail::g_thread_context.tls_device->device);
    if (did_something)
      return true;
    else
      return false;
  } else {
    // We are not assigned a thread-local device.
    // We are the main thread. Make progress on all devices.
    bool did_progress = false;
    for (auto &device : m_devices) {
      auto did_something = lcw::do_progress(device.device);
      if (did_something)
        did_progress = true;
    }
    return did_progress;
  }
}

void CommBackendLCW::barrier(void) {
  // nonblocking barrier
  lcw::barrier(getThreadLocalDevice()->device);
}

device_t *CommBackendLCW::getThreadLocalDevice() {
  if (!detail::g_thread_context.tls_device) {
    CmiAssert(m_devices.size() > 0);
    return &m_devices[0];
  } else {
    return detail::g_thread_context.tls_device;
  }
}

} // namespace lcw_impl
} // namespace comm_backend
