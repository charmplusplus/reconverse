#include "converse_internal.h"

#define LCI_SAFECALL(stmt)                                                    \
  do {                                                                        \
    int lci_errno = (stmt);                                                   \
    if (LCI_OK != lci_errno) {                                                \
      fprintf(stderr, "LCI call failed with %d \n", lci_errno);               \
      abort();                                                                \
    }                                                                         \
  } while (0)

namespace comm_backend {

std::vector<CompHandler> g_handlers;

void local_callback(LCI_request_t request)
{
  auto handler = reinterpret_cast<CompHandler>(request.user_context);
  void *address;
  size_t size;
  if (request.type == LCI_MEDIUM) {
    address = request.data.mbuffer.address;
    size = request.data.mbuffer.length;
  } else {
    address = request.data.lbuffer.address;
    size = request.data.lbuffer.length;
  }
  handler({static_cast<char*>(address), size});
}

void remote_callback(LCI_request_t request)
{
  auto am_handler = static_cast<AmHandler>(request.tag);
  auto handler = g_handlers[am_handler];
  size_t size;
  if (request.type == LCI_MEDIUM) {
    size = request.data.mbuffer.length;
  } else {
    size = request.data.lbuffer.length;
  }
  void *address = malloc(size);
  if (request.type == LCI_MEDIUM) {
    std::memcpy(address, request.data.mbuffer.address, size);
    LCI_mbuffer_free(request.data.mbuffer);
  } else {
    std::memcpy(address, request.data.lbuffer.address, size);
    LCI_lbuffer_free(request.data.lbuffer);
  }
  handler({static_cast<char*>(address), size});
}

void *CommBackendLCI1::alloc_mempool_block(size_t *size, mem_handle_t *mem_hndl, int expand_flag)
{
    size_t alloc_size =  expand_flag ? mempool_expand_size : mempool_init_size;
    if (*size < alloc_size) *size = alloc_size;
    if (*size > mempool_max_size)
    {
        CmiPrintf("Error: there is attempt to allocate memory block with size %ld which is greater than the maximum mempool allowed %lld.\n"
                  "Please increase the maximum mempool size by using +ofi-mempool-max-size\n",
                  *size, context.mempool_max_size);
        CmiAbort("alloc_mempool_block");
    }

    void *pool;
    posix_memalign(&pool,ALIGNBUF,*size);
    registerMemory(pool, *size);
    return pool;
}

void CommBackendLCI1::free_mempool_block(void *ptr, mem_handle_t mem_hndl)
{
    free(ptr);
    deregisterMemory((mr_t) mem_hndl);
}

void CommBackendLCI1::init_mempool()
{
  CpvInitialize(mempool_type*, mempool);

  mempool_init_size = MEMPOOL_INIT_SIZE_MB_DEFAULT * ONE_MB;
  mempool_expand_size = MEMPOOL_EXPAND_SIZE_MB_DEFAULT * ONE_MB;
  mempool_max_size = MEMPOOL_MAX_SIZE_MB_DEFAULT * ONE_MB;
  mempool_lb_size = MEMPOOL_LB_DEFAULT;
  mempool_rb_size = MEMPOOL_RB_DEFAULT;

  CpvAccess(mempool) = mempool_init(mempool_init_size,
    alloc_mempool_block,
    free_mempool_block,
    mempool_max_size);
}

void* CommBackendLCI1::malloc(int nbytes, int header)
{
  char *ptr = NULL;
  size_t size = n_bytes + header;
  if (size <= mempool_lb_size)
  {
    CmiAbort("OFI pool lower boundary violation");
  }
  else
  {
    CmiAssert(header+sizeof(mempool_header) <= ALIGNBUF);
    n_bytes=ALIGN64(n_bytes);
    if( n_bytes < BIG_MSG)
      {
        char *res = (char *)mempool_malloc(CpvAccess(mempool), ALIGNBUF+n_bytes, 1);

        // note CmiAlloc wrapper will move the pointer past the header
        if (res) ptr = res;

        size_t offset1=GetMemOffsetFromBase(ptr+header);
        mr_t* extractedmr  = (mr_t *) GetMemHndl(ptr+header);
      }
    else
      {
        n_bytes = size+ sizeof(out_of_pool_header);
        n_bytes = ALIGN64(n_bytes);
        char *res;

        posix_memalign((void **)&res, ALIGNBUF, n_bytes);
        out_of_pool_header *mptr= (out_of_pool_header*) res;
        // construct the minimal version of the
        // mempool_header+block_header like a memory pool message
        // so that all messages can be handled the same way with
        // the same macros and functions.  We need the mptr,
        // block_ptr, and mem_hndl fields and can test the size to
        // know to not put it back in the normal pool on free
        mr_t mr = registerMemory(res, n_bytes);
        mptr->block_head.mem_hndl=mr;
        mptr->block_head.mptr=(struct mempool_type*) res;
        mptr->block.block_ptr=(struct block_header *)res;
        ptr=(char *) res + (sizeof(out_of_pool_header));
  }

  if (!ptr) CmiAbort("LrtsAlloc");
  return ptr;
}

void free(void *msg)
{
  int headersize = sizeof(CmiChunkHeader);
  char *aligned_addr = (char *)msg + headersize - ALIGNBUF;
  uint size = SIZEFIELD((char*)msg+headersize);
  if (size <= context.mempool_lb_size)
    CmiAbort("LCI: mempool lower boundary violation");
  else
    size = ALIGN64(size);
  if(size>=BIG_MSG)
  {
    deregisterMemory( (mr_t)GetMemHndl( (char* )msg  +sizeof(CmiChunkHeader)));
    free((char *)msg-sizeof(out_of_pool_header));
  }
  else
  {
    mempool_free_thread(msg);
  }
}

void CommBackendLCI1::init(int *argc, char ***argv)
{
  init_mempool();
  int initialized = false;
  LCI_SAFECALL(LCI_initialized(&initialized));
  if (!initialized)
    LCI_SAFECALL(LCI_initialize());

  LCI_SAFECALL(LCI_handler_create(LCI_UR_DEVICE, local_callback, &m_local_comp));
  LCI_SAFECALL(LCI_handler_create(LCI_UR_DEVICE, remote_callback, &m_remote_comp));
  LCI_plist_t plist;
  LCI_SAFECALL(LCI_plist_create(&plist));
  LCI_SAFECALL(LCI_plist_set_comp_type(plist, LCI_PORT_COMMAND, LCI_COMPLETION_HANDLER));
  LCI_SAFECALL(LCI_plist_set_comp_type(plist, LCI_PORT_MESSAGE, LCI_COMPLETION_HANDLER));
  LCI_SAFECALL(LCI_plist_set_default_comp(plist, m_remote_comp));
  LCI_SAFECALL(LCI_endpoint_init(&m_ep, LCI_UR_DEVICE, plist));
  LCI_SAFECALL(LCI_plist_free(&plist));
}

void CommBackendLCI1::exit()
{
  LCI_SAFECALL(LCI_endpoint_free(&m_ep));
  LCI_SAFECALL(LCI_finalize());
}

int CommBackendLCI1::getMyNodeId()
{
  return LCI_RANK;
}

int CommBackendLCI1::getNumNodes()
{
  return LCI_NUM_PROCESSES;
}

AmHandler CommBackendLCI1::registerAmHandler(CompHandler handler)
{
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

void CommBackendLCI1::sendAm(int rank, void *msg, size_t size, mr_t mr,
                             CompHandler localComp, AmHandler remoteComp) {
  // we use LCI tag to pass the remoteComp
  LCI_error_t ret;
  do {
    if (size <= LCI_MEDIUM_SIZE) {
      LCI_mbuffer_t buffer;
      buffer.address = msg;
      buffer.length = size;
      ret = LCI_putma(m_ep, buffer, rank, remoteComp, LCI_DEFAULT_COMP_REMOTE);
      if (ret == LCI_OK) {
        // eager put is immediately completed.
        localComp({msg, size});
      }
    } else {
      LCI_lbuffer_t buffer;
      buffer.address = msg;
      buffer.length = size;
      buffer.segment =
          mr == MR_NULL ? LCI_SEGMENT_ALL : static_cast<LCI_segment_t>(mr);
      ret = LCI_putla(m_ep, buffer, m_local_comp, rank, remoteComp, LCI_DEFAULT_COMP_REMOTE, reinterpret_cast<void*>(localComp));
    }
    if (ret == LCI_ERR_RETRY) {
      progress();
    }
  } while (ret == LCI_ERR_RETRY);
}

bool CommBackendLCI1::progress(void)
{
  auto ret = LCI_progress(LCI_UR_DEVICE);
  return ret == LCI_OK;
}

void CommBackendLCI1::barrier(void)
{
  LCI_SAFECALL(LCI_barrier());
}

mr_t CommBackendLCI1::registerMemory(void *addr, size_t size)
{
  LCI_segment_t mr;
  LCI_SAFECALL(LCI_memory_register(LCI_UR_DEVICE, addr, size, &mr));
  return mr;
}

void CommBackendLCI1::deregisterMemory(mr_t mr)
{
  LCI_segment_t segment = static_cast<LCI_segment_t>(mr);
  LCI_SAFECALL(LCI_memory_deregister(&segment));
}

} // namespace comm_backend
