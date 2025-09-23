#ifndef COMM_BACKEND_INTERNAL_H
#define COMM_BACKEND_INTERNAL_H

#include <cstdio>
#include <vector>

#include "comm_backend/comm_backend.h"
#include "converse_config.h"

namespace comm_backend {

class CommBackendBase {
public:
  virtual void init(char **argv) = 0;
  virtual void exit() = 0;
  virtual void initThread(int thread_id, int num_threads) {}
  virtual void exitThread() {}
  virtual int getMyNodeId() = 0;
  virtual int getNumNodes() = 0;
  virtual bool isRMACapable() { return false; }
  virtual AmHandler registerAmHandler(CompHandler handler) = 0;
  virtual void issueAm(int rank, const void *local_buf, size_t size, mr_t mr,
                       CompHandler localComp, AmHandler remoteComp, void *user_context) = 0;
  virtual void issueRget(int rank, const void *local_buf, size_t size,
                         mr_t local_mr, uintptr_t remote_disp, void *rmr,
                         CompHandler localComp, void *user_context) {
    // Default implementation: not supported
    CmiAbort("Rget not supported in this backend");
  }
  virtual void issueRput(int rank, const void *local_buf, size_t size,
                         mr_t local_mr, uintptr_t remote_disp, void *rmr,
                         CompHandler localComp, void *user_context) {
    // Default implementation: not supported
    CmiAbort("Rput not supported in this backend");
  }
  // return true if there is more work to do
  virtual bool progress(void) = 0;
  virtual void barrier(void) = 0;
  virtual mr_t registerMemory(void *addr, size_t size) { return MR_NULL; }
  virtual size_t getRMR(mr_t mr, void *addr, size_t size) { return 0; }
  virtual void deregisterMemory(mr_t mr) {}
  virtual ~CommBackendBase() {};
};

} // namespace comm_backend

#ifdef RECONVERSE_ENABLE_COMM_LCI2
#include "comm_backend/lci2/comm_backend_lci2.h"
#endif
#ifdef RECONVERSE_ENABLE_COMM_LCW
#include "comm_backend/lcw/comm_backend_lcw.h"
#endif

#endif // COMM_BACKEND_INTERNAL_H
