#include "comm_backend/comm_backend_internal.h"

namespace comm_backend {

CommBackendBase *gCommBackend = nullptr;
int gNumNodes = 1;
int gMyNodeID = 0;

namespace
{
// A process started to join a running job begins as a job of one, and grows
// into the cluster when a membership change admits it. Its backend has to
// survive that: the single-node shortcut below would otherwise tear down the
// very endpoint whose address the coordinator is waiting to hand out.
bool willJoinLargerJob(char **argv)
{
  for (int i = 0; argv != nullptr && argv[i] != nullptr; i++) {
    if (strcmp(argv[i], "+coordinator") == 0) return true;
  }
  return false;
}
}  // namespace

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
  if (gNumNodes == 1 && !willJoinLargerJob(argv)) {
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
               void* remote_buf, void *rmr, CompHandler localComp, void *user_context) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->issueRget(rank, local_buf, size, local_mr, remote_buf, rmr,
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
  if (gCommBackend) {
    if (void *p = gCommBackend->malloc(nbytes, header)) {
      return p;
    }
  }
  void *p = std::malloc(nbytes + header);
  if (p != nullptr) {
    // Mark fallback allocations so comm_backend::free can dispatch correctly.
    static_cast<CmiChunkHeader *>(p)->mr = MR_NULL;
  }
  return p;
}

void free(void *msg)
{
  if (msg == nullptr) {
    return;
  }
  if (gCommBackend == nullptr || MRFIELD(msg) == MR_NULL) {
    std::free(static_cast<char *>(msg) - sizeof(CmiChunkHeader));
    return;
  }
  gCommBackend->free(msg);
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

void drain(void) {
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->drain();
}

bool supportsRescale(void) {
  if (gCommBackend == nullptr) {
    return false;
  }
  return gCommBackend->supportsRescale();
}

std::vector<unsigned char> getMyAddress(void) {
  if (gCommBackend == nullptr) {
    return {};
  }
  return gCommBackend->getMyAddress();
}

const std::vector<Member> &getMembers(void) {
  static const std::vector<Member> empty;
  if (gCommBackend == nullptr) {
    return empty;
  }
  return gCommBackend->getMembers();
}

bool coordBootstrap(const char *coordHost, int coordPort, int myNodeId,
                    int numNodes, bool isNewcomer) {
  if (gCommBackend == nullptr) {
    return false;
  }
  return gCommBackend->coordBootstrap(coordHost, coordPort, myNodeId, numNodes,
                                      isNewcomer);
}

void reconfigure(const ClusterView &view) {
  if (gCommBackend == nullptr) {
    CmiAbort("comm_backend::reconfigure with no backend");
  }
  gCommBackend->reconfigure(view);
  // The view is authoritative for the process's identity from here on.
  gMyNodeID = view.nodeId;
  gNumNodes = (int)view.members.size();
}

} // namespace comm_backend
