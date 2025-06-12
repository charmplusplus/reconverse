#include "converse_internal.h"

namespace comm_backend {

std::vector<CompHandler> g_handlers;

void local_callback(lci::status_t status) {
  auto handler = reinterpret_cast<CompHandler>(status.user_context);
  void *address;
  size_t size;
  lci::buffer_t buffer = status.data.get_buffer();
  handler({buffer.base, buffer.size});
}

void remote_callback(lci::status_t status) {
  auto am_handler = static_cast<AmHandler>(status.tag);
  auto handler = g_handlers[am_handler];
  lci::buffer_t buffer = status.data.get_buffer();
  handler({buffer.base, buffer.size});
}

void CommBackendLCI2::init(int *argc, char ***argv) {
  lci::g_runtime_init();
  m_local_comp = lci::alloc_handler(local_callback);
  m_remote_comp = lci::alloc_handler(remote_callback);
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

void CommBackendLCI2::issueAm(int rank, void *local_buf, size_t size, mr_t mr,
                              CompHandler localComp, AmHandler remoteComp) {
  // we use LCI tag to pass the remoteComp
  lci::status_t status;
  do {
    status = lci::post_am_x(rank, local_buf, size, m_local_comp, m_rcomp)
                 .mr(mr)
                 .tag(remoteComp)
                 .user_context(reinterpret_cast<void *>(localComp))();
    lci::progress();
  } while (status.error.is_retry());
  if (status.error.is_done()) {
    localComp({local_buf, size});
  }
}

void CommBackendLCI2::issueRget(int rank, void *local_buf, size_t size,
                                mr_t local_mr, uintptr_t remote_disp, void *rmr,
                                CompHandler localComp) {
  lci::status_t status;
  do {
    status = lci::post_get_x(rank, local_buf, size, m_local_comp, remote_disp,
                             *static_cast<lci::rmr_t *>(rmr))
                 .mr(local_mr)
                 .user_context(reinterpret_cast<void *>(localComp))();
    lci::progress();
  } while (status.error.is_retry());
  if (status.error.is_done()) {
    localComp({local_buf, size});
  }
}

void CommBackendLCI2::issueRput(int rank, void *local_buf, size_t size,
                                mr_t local_mr, uintptr_t remote_disp, void *rmr,
                                CompHandler localComp) {
  lci::status_t status;
  do {
    status = lci::post_put_x(rank, local_buf, size, m_local_comp, remote_disp,
                             *static_cast<lci::rmr_t *>(rmr))
                 .mr(local_mr)
                 .user_context(reinterpret_cast<void *>(localComp))();
    lci::progress();
  } while (status.error.is_retry());
  if (status.error.is_done()) {
    localComp({local_buf, size});
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
