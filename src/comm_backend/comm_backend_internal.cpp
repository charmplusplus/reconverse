#include "converse_internal.h"

namespace comm_backend {

CommBackendBase *gCommBackend = nullptr;
int gNumNodes = 1;
int gMyNodeID = 0;

void init(int *argc, char ***argv) 
{
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

void sendAm(int rank, void* msg, size_t size, mr_t mr, CompHandler localComp, AmHandler remoteComp)
{
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->sendAm(rank, msg, size, mr, localComp, remoteComp);
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

mr_t registerMemory(void* addr, size_t size)
{
  if (gCommBackend == nullptr) {
    return nullptr;
  }
  return gCommBackend->registerMemory(addr, size);
}

void deregisterMemory(mr_t mr)
{
  if (gCommBackend == nullptr) {
    return;
  }
  gCommBackend->deregisterMemory(mr);
}

} // namespace comm_backend
