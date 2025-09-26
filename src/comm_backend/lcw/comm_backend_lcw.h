#ifndef RECONVERSE_COMM_BACKEND_LCW_H
#define RECONVERSE_COMM_BACKEND_LCW_H

#include "lcw.hpp"

namespace comm_backend {
namespace lcw_impl {
struct AllocatorLCW : lcw::allocator_base_t {
  void *allocate(size_t size) override { return CmiAlloc(size); }

  void deallocate(void *ptr) override { CmiFree(ptr); }
};

struct device_t {
  lcw::device_t device;
  std::atomic<lcw::tag_t> next_rdv_tag;
  lcw::tag_t max_rdv_tag;
  device_t() : device(nullptr), next_rdv_tag(0), max_rdv_tag(0) {}
  bool is_empty() const { return device == nullptr; }
  lcw::tag_t getNextRdvTag() {
    return next_rdv_tag.fetch_add(1, std::memory_order_relaxed) % max_rdv_tag;
  }
};

class CommBackendLCW : public CommBackendBase {
public:
  void init(char **argv) override;
  void exit() override;
  void initThread(int thread_id, int num_threads) override;
  void exitThread() override;
  int getMyNodeId() override;
  int getNumNodes() override;
  AmHandler registerAmHandler(CompHandler handler) override;
  void issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
               CompHandler localComp, AmHandler remoteComp,
               void *user_context) override;
  bool progress(void) override;
  void barrier(void) override;

private:
  struct threadContext {
    int thread_id;
    device_t device;
  };

  std::vector<device_t> m_devices;
  lcw::comp_t m_local_comp;
  lcw::comp_t m_remote_comp;
  AllocatorLCW m_allocator;

  size_t eager_threshold;

  device_t *getThreadLocalDevice();

  friend void remoteRdvHeaderCallback(lcw::request_t request);
};

} // namespace lcw_impl
} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCW_H
