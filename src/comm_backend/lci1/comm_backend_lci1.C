#include "converse_internal.h"

#define LCI_SAFECALL(stmt)                                                    \
  do {                                                                        \
    int lci_errno = (stmt);                                                   \
    if (LCI_OK != lci_errno) {                                                \
      fprintf(stderr, "LCI call failed with %d \n", lci_errno);               \
      abort();                                                                \
    }                                                                         \
  } while (0)

namespace comm_backend {

std::vector<CompHandler> g_handlers;

void local_callback(LCI_request_t request)
{
  auto handler = reinterpret_cast<CompHandler>(request.user_context);
  void *address;
  size_t size;
  if (request.type == LCI_MEDIUM) {
    address = request.data.mbuffer.address;
    size = request.data.mbuffer.length;
  } else {
    address = request.data.lbuffer.address;
    size = request.data.lbuffer.length;
  }
  handler({static_cast<char*>(address), size});
}

void remote_callback(LCI_request_t request)
{
  auto am_handler = static_cast<AmHandler>(request.tag);
  auto handler = g_handlers[am_handler];
  size_t size;
  if (request.type == LCI_MEDIUM) {
    size = request.data.mbuffer.length;
  } else {
    size = request.data.lbuffer.length;
  }
  void *address = malloc(size);
  if (request.type == LCI_MEDIUM) {
    std::memcpy(address, request.data.mbuffer.address, size);
    LCI_mbuffer_free(request.data.mbuffer);
  } else {
    std::memcpy(address, request.data.lbuffer.address, size);
    LCI_lbuffer_free(request.data.lbuffer);
  }
  handler({static_cast<char*>(address), size});
}

void CommBackendLCI1::init(int *argc, char ***argv)
{
  int initialized = false;
  LCI_SAFECALL(LCI_initialized(&initialized));
  if (!initialized)
    LCI_SAFECALL(LCI_initialize());

  LCI_SAFECALL(LCI_handler_create(LCI_UR_DEVICE, local_callback, &m_local_comp));
  LCI_SAFECALL(LCI_handler_create(LCI_UR_DEVICE, remote_callback, &m_remote_comp));
  LCI_plist_t plist;
  LCI_SAFECALL(LCI_plist_create(&plist));
  LCI_SAFECALL(LCI_plist_set_comp_type(plist, LCI_PORT_COMMAND, LCI_COMPLETION_HANDLER));
  LCI_SAFECALL(LCI_plist_set_comp_type(plist, LCI_PORT_MESSAGE, LCI_COMPLETION_HANDLER));
  LCI_SAFECALL(LCI_plist_set_default_comp(plist, m_remote_comp));
  LCI_SAFECALL(LCI_endpoint_init(&m_ep, LCI_UR_DEVICE, plist));
  LCI_SAFECALL(LCI_plist_free(&plist));
}

void CommBackendLCI1::exit()
{
  LCI_SAFECALL(LCI_endpoint_free(&m_ep));
  LCI_SAFECALL(LCI_finalize());
}

int CommBackendLCI1::getMyNodeId()
{
  return LCI_RANK;
}

int CommBackendLCI1::getNumNodes()
{
  return LCI_NUM_PROCESSES;
}

AmHandler CommBackendLCI1::registerAmHandler(CompHandler handler)
{
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

void CommBackendLCI1::sendAm(int rank, void *msg, size_t size, mr_t mr,
                             CompHandler localComp, AmHandler remoteComp) {
  // we use LCI tag to pass the remoteComp
  LCI_error_t ret;
  do {
    if (size <= LCI_MEDIUM_SIZE) {
      LCI_mbuffer_t buffer;
      buffer.address = msg;
      buffer.length = size;
      ret = LCI_putma(m_ep, buffer, rank, remoteComp, LCI_DEFAULT_COMP_REMOTE);
      if (ret == LCI_OK) {
        // eager put is immediately completed.
        localComp({msg, size});
      }
    } else {
      LCI_lbuffer_t buffer;
      buffer.address = msg;
      buffer.length = size;
      buffer.segment =
          mr == MR_NULL ? LCI_SEGMENT_ALL : static_cast<LCI_segment_t>(mr);
      ret = LCI_putla(m_ep, buffer, m_local_comp, rank, remoteComp, LCI_DEFAULT_COMP_REMOTE, reinterpret_cast<void*>(localComp));
    }
    if (ret == LCI_ERR_RETRY) {
      progress();
    }
  } while (ret == LCI_ERR_RETRY);
}

bool CommBackendLCI1::progress(void)
{
  auto ret = LCI_progress(LCI_UR_DEVICE);
  return ret == LCI_OK;
}

void CommBackendLCI1::barrier(void)
{
  LCI_SAFECALL(LCI_barrier());
}

mr_t CommBackendLCI1::registerMemory(void *addr, size_t size)
{
  LCI_segment_t mr;
  LCI_SAFECALL(LCI_memory_register(LCI_UR_DEVICE, addr, size, &mr));
  return mr;
}

void CommBackendLCI1::deregisterMemory(mr_t mr)
{
  LCI_segment_t segment = static_cast<LCI_segment_t>(mr);
  LCI_SAFECALL(LCI_memory_deregister(&segment));
}

} // namespace comm_backend