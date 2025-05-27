#include "mempool.h"
#include "lci.hpp"
#include "comm_backend/lci2/comm_backend_lci2.h"
#include <vector>

CpvDeclare(mempool_type*, mempool);

namespace comm_backend {

std::vector<CompHandler> g_handlers;

void local_callback(lci::status_t status) {
  auto handler = reinterpret_cast<CompHandler>(status.user_context);
  void *address;
  size_t size;
  lci::buffer_t buffer = status.data.get_buffer();
  handler({buffer.base, buffer.size});
}

void remote_callback(lci::status_t status) {
  auto am_handler = static_cast<AmHandler>(status.tag);
  auto handler = g_handlers[am_handler];
  lci::buffer_t buffer = status.data.get_buffer();
  handler({buffer.base, buffer.size});
}

void *alloc_mempool_block(size_t *size, void **mem_hndl, int expand_flag)
{
    size_t alloc_size =  expand_flag ? mempool_options.mempool_expand_size : mempool_options.mempool_init_size;
    if (*size < alloc_size) *size = alloc_size;
    if (*size > mempool_options.mempool_max_size)
    {
        CmiPrintf("Error: there is attempt to allocate memory block with size %ld which is greater than the maximum mempool allowed %lld.\n"
                  "Please increase the maximum mempool size by using +ofi-mempool-max-size\n",
                  *size, mempool_options.mempool_max_size);
        CmiAbort("alloc_mempool_block");
    }

    void *pool;
    posix_memalign(&pool,ALIGNBUF,*size);
    registerMemory(pool, *size);
    return pool;
}

void free_mempool_block(void *ptr, void* mem_hndl)
{
    free(ptr);
    deregisterMemory((mr_t) mem_hndl);
}

void init_mempool()
{
  CpvInitialize(mempool_type*, mempool);

  CpvAccess(mempool) = mempool_init(mempool_options.mempool_init_size,
    alloc_mempool_block,
    free_mempool_block,
    mempool_options.mempool_max_size);
}

void* CommBackendLCI2::malloc(int n_bytes, int header)
{
  char *ptr = NULL;
  size_t size = n_bytes + header;
  if (size <= mempool_options.mempool_lb_size)
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
    }

  if (!ptr) CmiAbort("LrtsAlloc");
  return ptr;
}

void CommBackendLCI2::free(void *msg)
{
  int headersize = sizeof(CmiChunkHeader);
  char *aligned_addr = (char *)msg + headersize - ALIGNBUF;
  uint size = SIZEFIELD((char*)msg+headersize);
  if (size <= mempool_options.mempool_lb_size)
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

void CommBackendLCI2::init(int *argc, char ***argv) {
  lci::g_runtime_init();
  m_local_comp = lci::alloc_handler(local_callback);
  m_remote_comp = lci::alloc_handler(remote_callback);
  m_rcomp = lci::register_rcomp(m_remote_comp);
  lci::set_allocator(&m_allocator);
  lci::barrier();
}

void CommBackendLCI2::exit() {
  lci::barrier();
  lci::free_comp(&m_local_comp);
  lci::free_comp(&m_remote_comp);
  lci::g_runtime_fina();
}

int CommBackendLCI2::getMyNodeId() { return lci::get_rank_me(); }

int CommBackendLCI2::getNumNodes() { return lci::get_rank_n(); }

AmHandler CommBackendLCI2::registerAmHandler(CompHandler handler) {
  g_handlers.push_back(handler);
  return g_handlers.size() - 1;
}

void CommBackendLCI2::sendAm(int rank, void *msg, size_t size, mr_t mr,
                             CompHandler localComp, AmHandler remoteComp) {
  // we use LCI tag to pass the remoteComp
  lci::status_t status;
  do {
    status = lci::post_am_x(rank, msg, size, m_local_comp, m_rcomp)
                 .mr(mr)
                 .tag(remoteComp)
                 .user_context(reinterpret_cast<void *>(localComp))();
    lci::progress();
  } while (status.error.is_retry());
  if (status.error.is_done()) {
    localComp({msg, size});
  }
}

bool CommBackendLCI2::progress(void) {
  auto ret = lci::progress();
  return ret.is_done();
}

void CommBackendLCI2::barrier(void) { lci::barrier(); }

mr_t CommBackendLCI2::registerMemory(void *addr, size_t size) {
  auto mr = lci::register_memory(addr, size);
  return mr.get_impl();
}

void CommBackendLCI2::deregisterMemory(mr_t mr_) {
  lci::mr_t mr(mr);
  lci::deregister_memory(&mr);
}

} // namespace comm_backend
