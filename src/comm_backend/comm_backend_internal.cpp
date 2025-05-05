#include "converse_internal.h"

namespace comm_backend {

CommBackendBase *gCommBackend = nullptr;
int gNumNodes = 1;
int gMyNodeID = 0;

void init(int *argc, char ***argv) {
#ifdef RECONVERSE_ENABLE_COMM_LCI2
  gCommBackend = new CommBackendLCI2();
#elif defined(RECONVERSE_ENABLE_COMM_LCI1)
  gCommBackend = new CommBackendLCI1();
#endif
  if (gCommBackend == nullptr) {
    return;
  }

  gCommBackend->init(argc, argv);
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

int getMyNodeId() { return gMyNodeID; }

int getNumNodes() { return gNumNodes; }

AmHandler registerAmHandler(CompHandler handler) {
  if (gCommBackend == nullptr) {
    return -1;
  }
  return gCommBackend->registerAmHandler(handler);
}

void issueAm(int rank, void *msg, size_t size, mr_t mr, CompHandler localComp,
             AmHandler remoteComp) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueAm(rank, msg, size, mr, localComp, remoteComp);
}

void issueRget(int rank, void *local_buf, size_t size, mr_t local_mr,
               uintptr_t remote_buf, rkey_t rkey, CompHandler localComp) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueRget(rank, local_buf, size, local_mr, remote_buf, rkey,
                          localComp);
}

void issueRput(int rank, void *local_buf, size_t size, mr_t local_mr,
               uintptr_t remote_buf, rkey_t rkey, CompHandler localComp) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueRput(rank, local_buf, size, local_mr, remote_buf, rkey,
                          localComp);
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

rkey_t getRKey(mr_t mr) {
  if (gCommBackend == nullptr) {
    return 0;
  }
  return gCommBackend->getRKey(mr);
}

void deregisterMemory(mr_t mr) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->deregisterMemory(mr);
}

} // namespace comm_backend
