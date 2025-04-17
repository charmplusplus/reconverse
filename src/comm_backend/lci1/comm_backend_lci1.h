#ifndef RECONVERSE_COMM_BACKEND_LCI1_H
#define RECONVERSE_COMM_BACKEND_LCI1_H

#include "lci.h"

namespace comm_backend
{

class CommBackendLCI1 : public CommBackendBase
{
 public:
  void init(int *argc, char ***argv) override;
  void exit() override;
  int getMyNodeId() override;
  int getNumNodes() override;
  AmHandler registerAmHandler(CompHandler handler) override;
  void sendAm(int rank, void* msg, size_t size, mr_t mr, CompHandler localComp, AmHandler remoteComp) override;
  bool progress(void) override;
  void barrier(void) override;
  mr_t registerMemory(void *addr, size_t size) override;
  void deregisterMemory(mr_t mr) override;
 private:
  LCI_comp_t m_local_comp;
  LCI_comp_t m_remote_comp;
  LCI_endpoint_t m_ep;
};

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI1_H