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
  lci::barrier();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
  lci::g_runtime_fina();
}

int CommBackendLCI2::getMyNodeId() { return lci::get_rank_me(); }

int CommBackendLCI2::getNumNodes() { return lci::get_rank_n(); }

AmHandler CommBackendLCI2::registerAmHandler(CompHandler handler) {
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

void CommBackendLCI2::sendAm(int rank, void *msg, size_t size, mr_t mr,
                             CompHandler localComp, AmHandler remoteComp) {
  // we use LCI tag to pass the remoteComp
  lci::status_t status;
  do {
    status = lci::post_am_x(rank, msg, size, m_local_comp, m_rcomp)
                 .mr(mr)
                 .tag(remoteComp)
                 .user_context(reinterpret_cast<void *>(localComp))();
    lci::progress();
  } while (status.error.is_retry());
  if (status.error.is_done()) {
    localComp({msg, size});
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

void CommBackendLCI2::deregisterMemory(mr_t mr_) {
  lci::mr_t mr(mr);
  lci::deregister_memory(&mr);
}

} // namespace comm_backend
