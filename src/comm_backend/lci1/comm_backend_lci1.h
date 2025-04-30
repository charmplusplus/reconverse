#ifndef RECONVERSE_COMM_BACKEND_LCI1_H
#define RECONVERSE_COMM_BACKEND_LCI1_H

#include "lci.hpp"
#include "converse.h"

#define MEMPOOL_INIT_SIZE_MB_DEFAULT   64
#define MEMPOOL_EXPAND_SIZE_MB_DEFAULT 64
#define MEMPOOL_MAX_SIZE_MB_DEFAULT    512
#define MEMPOOL_LB_DEFAULT             0
#define MEMPOOL_RB_DEFAULT             134217728

#define ALIGNBUF (sizeof(mempool_header)+sizeof(CmiChunkHeader))
#define   GetMempoolBlockPtr(x)   MEMPOOL_GetBlockPtr(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMempoolPtr(x)        MEMPOOL_GetMempoolPtr(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))

#define   GetMempoolsize(x)       MEMPOOL_GetSize(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMemHndl(x)           MEMPOOL_GetMemHndl(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))

#define   GetMemHndlFromBlockHeader(x) MEMPOOL_GetBlockMemHndl(x)
#define   GetSizeFromBlockHeader(x)    MEMPOOL_GetBlockSize(x)
#define   GetBaseAllocPtr(x) GetMempoolBlockPtr(x)
#define   GetMemOffsetFromBase(x) ((char*)(x) - (char *) GetBaseAllocPtr(x))

#define ALIGN64(x)       (size_t)((~63)&((x)+63))
#define ONE_MB (1024ll*1024)
static int8_t BIG_MSG  =  16 * ONE_MB;

CpvDeclare(mempool_type*, mempool);

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

  void *malloc(int nbytes, int header);
  void *free(void* msg);
  void *alloc_mempool_block(size_t *size, mem_handle_t *mem_hndl, int expand_flag);
  void free_mempool_block(void *ptr, mem_handle_t mem_hndl);
  void init_mempool();
 private:
  LCI_comp_t m_local_comp;
  LCI_comp_t m_remote_comp;
  LCI_endpoint_t m_ep;

  size_t mempool_init_size;
  size_t mempool_expand_size;
  long long mempool_max_size;
  size_t mempool_lb_size;
  size_t mempool_rb_size;
};

} // namespace comm_backend

#endif // RECONVERSE_COMM_BACKEND_LCI1_H
