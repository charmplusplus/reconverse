#ifndef RECONVERSE_COMM_BACKEND_LCI1_H
#define RECONVERSE_COMM_BACKEND_LCI1_H

#include "lci.hpp"
#include "converse_internal.h"

namespace comm_backend {

class CommBackendLCI1 : public CommBackendBase {
public:
  void init(int *argc, char ***argv) override;
  void exit() override;
  int getMyNodeId() override;
  int getNumNodes() override;
  AmHandler registerAmHandler(CompHandler handler) override;
  void sendAm(int rank, void *msg, size_t size, mr_t mr, CompHandler localComp,
              AmHandler remoteComp) override;
  bool progress(void) override;
  void barrier(void) override;
  mr_t registerMemory(void *addr, size_t size) override;
  void deregisterMemory(mr_t mr) override;

  void *malloc(int nbytes, int header);
  void free(void* msg);
  void *alloc_mempool_block(size_t *size, mem_handle_t *mem_hndl, int expand_flag);
  void free_mempool_block(void *ptr, mem_handle_t mem_hndl);
  void init_mempool();

private:
  LCI_comp_t m_local_comp;
  LCI_comp_t m_remote_comp;
  LCI_endpoint_t m_ep;

  size_t mempool_init_size;
  size_t mempool_expand_size;
  long long mempool_max_size;
  size_t mempool_lb_size;
  size_t mempool_rb_size;
};

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI1_H
