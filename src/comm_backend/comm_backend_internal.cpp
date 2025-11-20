#include "comm_backend/comm_backend_internal.h"

namespace comm_backend {

CommBackendBase *gCommBackend = nullptr;
int gNumNodes = 1;
int gMyNodeID = 0;

void init(char **argv) {
  const char *backend_str = nullptr;
// default to LCI2 if both are enabled
#ifdef RECONVERSE_ENABLE_COMM_LCI2
  backend_str = "lci";
#elif defined(RECONVERSE_ENABLE_COMM_LCW)
  backend_str = "lcw";
#else
  backend_str = "none";
#endif
  char *backend_str_input = nullptr;
  CmiGetArgStringDesc(argv, "+backend", &backend_str_input,
                      "Communication backend to use {lci, lcw} ");
  if (backend_str_input) {
    backend_str = backend_str_input;
  }
  if (strcmp(backend_str, "lci") == 0) {
#ifdef RECONVERSE_ENABLE_COMM_LCI2
    gCommBackend = new lci2_impl::CommBackendLCI2();
#else
    CmiAbort("LCI2 backend is not enabled in this build.\n");
#endif
  } else if (strcmp(backend_str, "lcw") == 0) {
#ifdef RECONVERSE_ENABLE_COMM_LCW
    gCommBackend = new lcw_impl::CommBackendLCW();
#else
    CmiAbort("LCW backend is not enabled in this build.\n");
#endif
  } else if (strcmp(backend_str, "none") == 0) {
    return;
  } else {
    CmiAbort("Unknown communication backend: %s\n", backend_str);
  }

  gCommBackend->init(argv);
  gMyNodeID = gCommBackend->getMyNodeId();
  gNumNodes = gCommBackend->getNumNodes();
  if (gNumNodes == 1) {
    //DEBUGF("Only one node detected, exiting comm backend\n");
    exit();
  }
}

void init_mempool() {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->init_mempool();
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

bool isRMACapable() {
  if (gCommBackend == nullptr) {
    return false;
  }
  return gCommBackend->isRMACapable();
}

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

void *malloc(int nbytes, int header)
{
  if (gCommBackend == nullptr) {
    return std::malloc(nbytes + header);
  }
  return gCommBackend->malloc(nbytes, header);
}

void free(void* msg)
{
  if (gCommBackend == nullptr) {
    std::free(msg - sizeof(CmiChunkHeader));
    return;
  }
  return gCommBackend->free(msg);
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
