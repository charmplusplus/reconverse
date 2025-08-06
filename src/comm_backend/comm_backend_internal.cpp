#include "converse_internal.h"

namespace comm_backend {

CommBackendBase *gCommBackend = nullptr;
int gNumNodes = 1;
int gMyNodeID = 0;

void init(char **argv) {
#ifdef RECONVERSE_ENABLE_COMM_LCI2
  gCommBackend = new CommBackendLCI2();
#endif
  if (gCommBackend == nullptr) {
    return;
  }

  gCommBackend->init(argv);
  gMyNodeID = gCommBackend->getMyNodeId();
  gNumNodes = gCommBackend->getNumNodes();
}

void exit() {
  if (gCommBackend) {
    gCommBackend->exit();
    delete gCommBackend;
    gCommBackend = nullptr;
  }
}

void initThread(int thread_id, int num_threads) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->initThread(thread_id, num_threads);
}

void exitThread() {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->exitThread();
}

int getMyNodeId() { return gMyNodeID; }

int getNumNodes() { return gNumNodes; }

AmHandler registerAmHandler(CompHandler handler) {
  if (gCommBackend == nullptr) {
    return -1;
  }
  return gCommBackend->registerAmHandler(handler);
}

void issueAm(int rank, const void *msg, size_t size, mr_t mr, CompHandler localComp,
             AmHandler remoteComp, void *user_context) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueAm(rank, msg, size, mr, localComp, remoteComp, user_context);
}

void issueRget(int rank, const void *local_buf, size_t size, mr_t local_mr,
               uintptr_t remote_disp, void *rmr, CompHandler localComp, void *user_context) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueRget(rank, local_buf, size, local_mr, remote_disp, rmr,
                          localComp, user_context);
}

void issueRput(int rank, const void *local_buf, size_t size, mr_t local_mr,
               uintptr_t remote_disp, void *rmr, CompHandler localComp, void *user_context) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueRput(rank, local_buf, size, local_mr, remote_disp, rmr,
                          localComp, user_context);
}

bool progress(void) {
  if (gCommBackend == nullptr) {
    return false;
  }
  return gCommBackend->progress();
}

void barrier(void) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->barrier();
}

mr_t registerMemory(void *addr, size_t size) {
  if (gCommBackend == nullptr) {
    return nullptr;
  }
  return gCommBackend->registerMemory(addr, size);
}

size_t getRMR(mr_t mr, void *addr, size_t size) {
  if (gCommBackend == nullptr) {
    return 0;
  }
  return gCommBackend->getRMR(mr, addr, size);
}

void deregisterMemory(mr_t mr) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->deregisterMemory(mr);
}

} // namespace comm_backend
