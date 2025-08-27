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
  void init(char **argv) override;
  void exit() override;
  void initThread(int thread_id, int num_threads) override;
  void exitThread() override;
  int getMyNodeId() override;
  int getNumNodes() override;
  AmHandler registerAmHandler(CompHandler handler) override;
  void issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
               CompHandler localComp, AmHandler remoteComp, void *user_context) override;
  void issueRget(int rank, const void *local_buf, size_t size, mr_t local_mr,
                 uintptr_t remote_disp, void *rmr,
                 CompHandler localComp, void *user_context) override;
  void issueRput(int rank, const void *local_buf, size_t size, mr_t local_mr,
                 uintptr_t remote_disp, void *rmr,
                 CompHandler localComp, void *user_context) override;
  bool progress(void) override;
  void barrier(void) override;
  mr_t registerMemory(void *addr, size_t size) override;
  size_t getRMR(mr_t mr, void *addr, size_t size) override;
  void deregisterMemory(mr_t mr) override;

private:
  struct threadContext {
    int thread_id;
    lci::device_t device;
  };

  std::vector<lci::device_t> m_devices;
  lci::comp_t m_local_comp;
  lci::comp_t m_remote_comp;
  lci::rcomp_t m_rcomp;
  AllocatorLCI2 m_allocator;

  lci::device_t getThreadLocalDevice();
  lci::mr_t getThreadLocalMR(mr_t mr);
  lci::rmr_t getThreadLocalRMR(void *rmr);
};

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI2_H
