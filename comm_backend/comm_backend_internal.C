#include "comm_backend/comm_backend_internal.h"

namespace comm_backend {

CommBackendBase *gCommBackend = nullptr;
int gNumNodes = 1;
int gMyNodeID = 0;

void init(int *argc, char ***argv) 
{
#ifdef RECONVERSE_ENABLE_COMM_LCI1
  gCommBackend = new CommBackendLCI1();
#endif
  if (gCommBackend == nullptr) {
    return;
  }
  
  gCommBackend->init(argc, argv);
  gMyNodeID = gCommBackend->getMyNodeId();
  gNumNodes = gCommBackend->getNumNodes();
}

void exit()
{
  if (gCommBackend) {
    gCommBackend->exit();
    delete gCommBackend;
    gCommBackend = nullptr;
  }
}

int getMyNodeId()
{
  return gMyNodeID;
}

int getNumNodes()
{
  return gNumNodes;
}

AmHandler registerAmHandler(CompHandler handler)
{
  if (gCommBackend == nullptr) {
    return -1;
  }
  return gCommBackend->registerAmHandler(handler);
}

void sendAm(int rank, void* msg, size_t size, CompHandler localComp, AmHandler remoteComp)
{
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->sendAm(rank, msg, size, localComp, remoteComp);
}

bool progress(void)
{
  if (gCommBackend == nullptr) {
    return false;
  }
  return gCommBackend->progress();
}

void barrier(void)
{
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->barrier();
}

} // namespace comm_backend