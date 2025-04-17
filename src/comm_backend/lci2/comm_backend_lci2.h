#ifndef RECONVERSE_COMM_BACKEND_LCI2_H
#define RECONVERSE_COMM_BACKEND_LCI2_H

#include "lci.hpp"

namespace comm_backend {
// A breach of the comm_backend interface with direct access to CmiAlloc/CmiFree
// There are another way to do this, but this is the simplest way to do it
struct AllocatorLCI2 : lci::allocator_base_t {
  void *allocate(size_t size) override { return CmiAlloc(size); }

  void deallocate(void *ptr) override { CmiFree(ptr); }
};

class CommBackendLCI2 : public CommBackendBase {
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

private:
  lci::comp_t m_local_comp;
  lci::comp_t m_remote_comp;
  lci::rcomp_t m_rcomp;
  AllocatorLCI2 m_allocator;
};

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI2_H