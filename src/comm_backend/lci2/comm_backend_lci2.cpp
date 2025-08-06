#include "converse_internal.h"

namespace comm_backend {

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
  m_local_comp = lci::alloc_handler(localCallback);
  m_remote_comp = lci::alloc_handler(remoteCallback);
  m_rcomp = lci::register_rcomp(m_remote_comp);
  lci::set_allocator(&m_allocator);
  lci::barrier();
}

void CommBackendLCI2::exit() {
  lci::g_runtime_fina();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
}

int CommBackendLCI2::getMyNodeId() { return lci::get_rank_me(); }

int CommBackendLCI2::getNumNodes() { return lci::get_rank_n(); }

AmHandler CommBackendLCI2::registerAmHandler(CompHandler handler) {
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

void CommBackendLCI2::issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
                              CompHandler localComp, AmHandler remoteComp, void *user_context) {
  // we use LCI tag to pass the remoteComp
  auto args = new localCallbackArgs{localComp, user_context};
  lci::status_t status;
  do {
    status = lci::post_am_x(rank, const_cast<void *>(local_buf), size, m_local_comp, m_rcomp)
                 .mr(mr)
                 .tag(remoteComp)
                 .user_context(args)();
    lci::progress();
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
    lci::progress();
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
    lci::progress();
  } while (status.is_retry());
  if (status.is_done()) {
    localComp({local_buf, size, user_context});
    delete args;
  }
}

bool CommBackendLCI2::progress(void) {
  auto ret = lci::progress();
  return ret.is_done();
}

void CommBackendLCI2::barrier(void) { lci::barrier(); }

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
