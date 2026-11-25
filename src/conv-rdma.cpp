/* Support for Direct Nocopy API (Generic Implementation)
 * Specific implementations are in arch/layer/machine-onesided.{h,c}
 */
#include "conv-rdma.h"
#include "converse_internal.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if CMK_CUDA
#include <cuda_runtime.h>
#elif CMK_HIP
#include <hip/hip_runtime.h>
#endif

// User specified configuration
// TODO: move to a better location
bool CmiUseCopyBasedRDMA = true;

bool useCMAForZC;
CpvExtern(std::vector<NcpyOperationInfo *>, newZCPupGets);
static int zc_pup_handler_idx;

void CmiSetNcpyAckSize(int ackSize) {}

void CmiForwardNodeBcastMsg(int size, char *msg) {}

void CmiForwardProcBcastMsg(int size, char *msg) {}

/****************************** Zerocopy Direct API For non-RDMA layers
 * *****************************/
/* Support for generic implementation */

// Function Pointer to Acknowledement handler function for the Direct API
RdmaAckCallerFn ncpyDirectAckHandlerFn;

// An Rget initiator PE sends this message to the target PE that will be the
// source of the data
typedef struct _converseRdmaMsg {
  char cmicore[CmiMsgHeaderSizeBytes];
} ConverseRdmaMsg;

static int remote_dereg_handler_idx;
static int get_request_handler_idx;
static int put_data_handler_idx;
static int put_done_handler_idx;
static int reg_and_put_handler_idx;
static int reg_and_get_handler_idx;
static int put_signal_msg_handler_idx;

// Invoked when this PE has to deregister the local memory
static void remoteDeregHandler(ConverseRdmaMsg *deregMsg) {
  NcpyOperationInfo *ncpyOpInfo = reinterpret_cast<NcpyOperationInfo *>(
      reinterpret_cast<char *>(deregMsg) + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  // The NcpyOperationInfo is embedded in deregMsg, so it is not an independent
  // allocation and must not be freed by the upper layer; deregMsg is released
  // at the end of this handler instead.
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;

  // Compare by node, not PE: in SMP the buffer's owning PE need not be the PE
  // that happens to run this handler.
  if (CmiMyNode() == CmiNodeOf(ncpyOpInfo->srcPe)) {
    CmiDeregisterMem(ncpyOpInfo->srcPtr,
                     ncpyOpInfo->srcLayerInfo + CmiGetRdmaCommonInfoSize(),
                     ncpyOpInfo->srcPe, ncpyOpInfo->srcRegMode);
    ncpyOpInfo->isSrcRegistered = 0;
    ncpyOpInfo->ackMode = CMK_SRC_ACK;
    // Downgrade opMode so the upper layer only invokes the source callback.
    // Leaving the original opMode (e.g. CMK_EM_API) would re-run the whole
    // completion path here, which would send another dereg message back and
    // loop forever, and would leak an unmatched QdCreate on every pass.
    ncpyOpInfo->opMode = CMK_EM_API_SRC_ACK_INVOKE;
  } else if (CmiMyNode() == CmiNodeOf(ncpyOpInfo->destPe)) {
    CmiDeregisterMem(ncpyOpInfo->destPtr,
                     ncpyOpInfo->destLayerInfo + CmiGetRdmaCommonInfoSize(),
                     ncpyOpInfo->destPe, ncpyOpInfo->destRegMode);
    ncpyOpInfo->isDestRegistered = 0;
    ncpyOpInfo->ackMode = CMK_DEST_ACK;
    ncpyOpInfo->opMode = CMK_EM_API_DEST_ACK_INVOKE;
  } else {
    CmiAbort("remoteDeregHandler: neither source nor destination node\n");
  }

  ncpyDirectAckHandlerFn(ncpyOpInfo);

  CmiFree(deregMsg);
}

// Invoked when this PE has to send a large array for an Rget
static void getRequestHandler(ConverseRdmaMsg *getReqMsg) {
  NcpyOperationInfo *ncpyOpInfo =
      (NcpyOperationInfo *)((char *)(getReqMsg) + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  // Mark the operation as a get served by a put: the initiator is the
  // destination, and the single acknowledgement belongs there (see
  // putDataHandler). A direct put keeps CMK_SRC_DEST_ACK.
  ncpyOpInfo->ackMode = CMK_DEST_ACK;

  // Get is implemented internally using a call to Put
  CmiIssueRput(ncpyOpInfo);
}

// Invoked when this PE receives a large array as the target of an Rput or the
// initiator of an Rget.
//
// One acknowledgement per operation, on the initiating PE, with
// CMK_SRC_DEST_ACK: the same contract as the RMA path (CommRgetLocalHandler,
// CommRputLocalHandler) and as classic's one-sided layers, which is what
// CMK_ONESIDED_IMPL in conv-rdma.h promises Charm++. Charm++ invokes the
// initiator's own callback from that ack and delivers the other side's
// through its deregistration round trip. For a get served by this put the
// initiator is this PE; for a direct put it is the source, which is told by a
// put-done message once the bytes have landed here, so its ack cannot run
// ahead of the data.
static void putDataHandler(ConverseRdmaMsg *payloadMsg) {
  NcpyOperationInfo *ncpyOpInfo =
      (NcpyOperationInfo *)((char *)payloadMsg + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  // copy the received messsage into the user's destination address
  memcpy((char *)ncpyOpInfo->destPtr,
         (char *)payloadMsg + sizeof(ConverseRdmaMsg) +
             ncpyOpInfo->ncpyOpInfoSize,
         std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize));

  // The NcpyOperationInfo lives inside payloadMsg: nothing downstream may free
  // it (CmiInvokeRemoteDeregAckHandler honors this).
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  if (ncpyOpInfo->ackMode == CMK_DEST_ACK) {
    // A get served by a put: this PE initiated it, acknowledge here.
    ncpyOpInfo->ackMode = CMK_SRC_DEST_ACK;
    ncpyDirectAckHandlerFn(ncpyOpInfo);
    CmiFree(payloadMsg);
  } else {
    // A direct put: the source initiated it; tell the source the data landed.
    int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;
    ConverseRdmaMsg *doneMsg =
        (ConverseRdmaMsg *)CmiAlloc(sizeof(ConverseRdmaMsg) + ncpyOpInfoSize);
    memcpy((char *)doneMsg + sizeof(ConverseRdmaMsg), (char *)ncpyOpInfo,
           ncpyOpInfoSize);
    CmiSetHandler(doneMsg, put_done_handler_idx);
    CmiSyncSendAndFree(ncpyOpInfo->srcPe,
                       sizeof(ConverseRdmaMsg) + ncpyOpInfoSize, doneMsg);
    CmiFree(payloadMsg);
  }
}

// Invoked on the source of a direct put once the destination has the data.
static void putDoneHandler(ConverseRdmaMsg *doneMsg) {
  NcpyOperationInfo *ncpyOpInfo =
      (NcpyOperationInfo *)((char *)doneMsg + sizeof(ConverseRdmaMsg));
  resetNcpyOpInfoPointers(ncpyOpInfo);
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  ncpyOpInfo->ackMode = CMK_SRC_DEST_ACK;
  ncpyDirectAckHandlerFn(ncpyOpInfo);
  CmiFree(doneMsg);
}

void RDMAInit(char **argv) {
  CmiUseCopyBasedRDMA = !comm_backend::isRMACapable();
  int force_copybased =
      CmiGetArgFlagDesc(argv, "+nordma", "Force use of copy-based RDMA");
  if (force_copybased) {
    CmiUseCopyBasedRDMA = true;
  }
}

void CmiIssueRgetCopyBased(NcpyOperationInfo *ncpyOpInfo) {
  int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;
  // Send a ConverseRdmaMsg to other PE requesting it to send the array
  ConverseRdmaMsg *getReqMsg =
      (ConverseRdmaMsg *)CmiAlloc(sizeof(ConverseRdmaMsg) + ncpyOpInfoSize);

  // copy the additional Info into the getReqMsg
  memcpy((char *)getReqMsg + sizeof(ConverseRdmaMsg), (char *)ncpyOpInfo,
         ncpyOpInfoSize);

  CmiSetHandler(getReqMsg, get_request_handler_idx);
  CmiSyncSendAndFree(ncpyOpInfo->srcPe,
                     sizeof(ConverseRdmaMsg) + ncpyOpInfoSize, getReqMsg);

  // free original ncpyOpinfo
  if (ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);
}

void CmiIssueRputCopyBased(NcpyOperationInfo *ncpyOpInfo) {
  int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;
  int size = ncpyOpInfo->srcSize;
  int destPe = ncpyOpInfo->destPe;

  // Send a ConverseRdmaMsg to the other PE sending the array
  ConverseRdmaMsg *payloadMsg = (ConverseRdmaMsg *)CmiAlloc(
      sizeof(ConverseRdmaMsg) + ncpyOpInfoSize + size);

  // copy the ncpyOpInfo into the recvMsg
  memcpy((char *)payloadMsg + sizeof(ConverseRdmaMsg), (char *)ncpyOpInfo,
         ncpyOpInfoSize);

  // copy the large array into the recvMsg
  memcpy((char *)payloadMsg + sizeof(ConverseRdmaMsg) + ncpyOpInfoSize,
         ncpyOpInfo->srcPtr, size);

  // No acknowledgement here. putDataHandler on the destination raises the
  // one ack of this operation: on itself when it initiated a get that this
  // put serves (ackMode == CMK_DEST_ACK, set by getRequestHandler), or back on
  // this PE through putDoneHandler for a direct put, once the bytes landed.
  NcpyOperationInfo *embedded =
      (NcpyOperationInfo *)((char *)payloadMsg + sizeof(ConverseRdmaMsg));
  if (embedded->ackMode != CMK_DEST_ACK) embedded->ackMode = CMK_SRC_DEST_ACK;
  if (ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);

  CmiSetHandler(payloadMsg, put_data_handler_idx); // putDataHandler
  CmiSyncSendAndFree(destPe, sizeof(ConverseRdmaMsg) + ncpyOpInfoSize + size,
                     payloadMsg);
}

void CommRputRemoteHandler(comm_backend::Status status);
void CommRgetRemoteHandler(comm_backend::Status status);

// Copy size bytes from src to dst, dispatching to the GPU runtime when either
// pointer is a device allocation.  Falls back to host-staged copies if the
// direct GPU transfer fails (e.g. peer access unavailable), and to plain
// memcpy when neither pointer is a device allocation or no GPU support is
// compiled in.
static void memcpyAnyPtr(void *dst, const void *src, size_t size) {
#if CMK_CUDA
  {
    cudaPointerAttributes srcAttr{}, dstAttr{};
    bool srcIsDevice = (cudaPointerGetAttributes(&srcAttr, src) == cudaSuccess &&
                        srcAttr.type == cudaMemoryTypeDevice);
    bool dstIsDevice = (cudaPointerGetAttributes(&dstAttr, dst) == cudaSuccess &&
                        dstAttr.type == cudaMemoryTypeDevice);
    if (srcIsDevice || dstIsDevice) {
      if (cudaMemcpy(dst, src, size, cudaMemcpyDefault) == cudaSuccess)
        return;
      // Host staging fallback (e.g. P2P not available between two devices)
      void *h = malloc(size);
      CmiAssert(h != nullptr);
      if (srcIsDevice)
        cudaMemcpy(h, src, size, cudaMemcpyDeviceToHost);
      else
        memcpy(h, src, size);
      if (dstIsDevice)
        cudaMemcpy(dst, h, size, cudaMemcpyHostToDevice);
      else
        memcpy(dst, h, size);
      free(h);
      return;
    }
  }
#elif CMK_HIP
  {
    hipPointerAttribute_t srcAttr{}, dstAttr{};
    bool srcIsDevice = (hipPointerGetAttributes(&srcAttr, src) == hipSuccess &&
                        srcAttr.type == hipMemoryTypeDevice);
    bool dstIsDevice = (hipPointerGetAttributes(&dstAttr, dst) == hipSuccess &&
                        dstAttr.type == hipMemoryTypeDevice);
    if (srcIsDevice || dstIsDevice) {
      if (hipMemcpy(dst, src, size, hipMemcpyDefault) == hipSuccess)
        return;
      // Host staging fallback
      void *h = malloc(size);
      CmiAssert(h != nullptr);
      if (srcIsDevice)
        hipMemcpy(h, src, size, hipMemcpyDeviceToHost);
      else
        memcpy(h, src, size);
      if (dstIsDevice)
        hipMemcpy(dst, h, size, hipMemcpyHostToDevice);
      else
        memcpy(dst, h, size);
      free(h);
      return;
    }
  }
#endif
  memcpy(dst, src, size);
}

static void regAndPutHandler(ConverseRdmaMsg *msg);
static void regAndGetHandler(ConverseRdmaMsg *msg);
static void putSignalMsgHandler(void *msg);

// Rget/Rput operations are implemented as normal converse messages
// This method is invoked during converse initialization to initialize these
// message handlers
void CmiOnesidedDirectInit(void) {
  remote_dereg_handler_idx = CmiRegisterHandler((CmiHandler)remoteDeregHandler);
  get_request_handler_idx = CmiRegisterHandler((CmiHandler)getRequestHandler);
  put_data_handler_idx = CmiRegisterHandler((CmiHandler)putDataHandler);
  put_done_handler_idx = CmiRegisterHandler((CmiHandler)putDoneHandler);
  reg_and_put_handler_idx = CmiRegisterHandler((CmiHandler)regAndPutHandler);
  reg_and_get_handler_idx = CmiRegisterHandler((CmiHandler)regAndGetHandler);
  zc_pup_handler_idx = CmiRegisterHandler((CmiHandler)zcPupHandler);
  put_signal_msg_handler_idx =
      CmiRegisterHandler((CmiHandler)putSignalMsgHandler);
}

/****************************** Zerocopy Direct API
 * *****************************/

// Get Methods
void CmiNcpyBuffer::memcpyGet(CmiNcpyBuffer &source) {
  // memcpy the data from the source buffer into the destination buffer
  memcpy((void *)ptr, source.ptr, std::min(cnt, source.cnt));
}

#if CMK_USE_CMA
void CmiNcpyBuffer::cmaGet(CmiNcpyBuffer &source) {
  CmiIssueRgetUsingCMA(source.ptr, source.layerInfo, source.pe, ptr, layerInfo,
                       pe, std::min(cnt, source.cnt));
}
#endif

// Registers an unregistered buffer for a network transfer that this side
// initiates. With CMK_BUFFER_NODEREG the registration stays with this handle
// until the user calls deregisterMem(), which clears isRegistered, so a handle
// that starts several transfers registers once. Registering again would
// overwrite layerInfo and lose the earlier registration, which nothing could
// then release (#331). With CMK_BUFFER_DEREG the layer above releases the
// registration through the operation's copy of layerInfo when the transfer
// completes, which leaves isRegistered here stale, so every transfer
// registers.
static void registerForTransfer(CmiNcpyBuffer &buf) {
  if (buf.regMode != CMK_BUFFER_UNREG)
    return;
  if (buf.isRegistered && buf.deregMode == CMK_BUFFER_NODEREG)
    return;
  CmiSetRdmaBufferInfo(buf.layerInfo + CmiGetRdmaCommonInfoSize(), buf.ptr,
                       buf.cnt, buf.regMode);
  buf.isRegistered = true;
}

void CmiNcpyBuffer::rdmaGet(CmiNcpyBuffer &source, int ackSize, char *srcAck,
                            char *destAck) {
  registerForTransfer(*this);

  int rootNode = -1; // -1 is the rootNode for p2p operations

  NcpyOperationInfo *ncpyOpInfo =
      createNcpyOpInfo(source, *this, ackSize, srcAck, destAck, rootNode,
                       CMK_DIRECT_API, (void *)ref);
  CmiIssueRget(ncpyOpInfo);
}

NcpyOperationInfo *CmiNcpyBuffer::createNcpyOpInfo(CmiNcpyBuffer &source,
                                                   CmiNcpyBuffer &destination,
                                                   int ackSize, char *srcAck,
                                                   char *destAck, int rootNode,
                                                   int opMode, void *refPtr) {
  int layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

  // Create a general object that can be used across layers and can store the
  // state of the CmiNcpyBuffer objects
  int ncpyObjSize =
      getNcpyOpInfoTotalSize(layerInfoSize, ackSize, layerInfoSize, ackSize);

  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyObjSize);

  setNcpyOpInfo(source.ptr, (char *)(source.layerInfo), layerInfoSize, srcAck,
                ackSize, source.cnt, source.regMode, source.deregMode,
                source.isRegistered, source.pe, source.ref, destination.ptr,
                (char *)(destination.layerInfo), layerInfoSize, destAck,
                ackSize, destination.cnt, destination.regMode,
                destination.deregMode, destination.isRegistered, destination.pe,
                destination.ref, rootNode, ncpyOpInfo, destination.deviceRdmaOpInfo);

  ncpyOpInfo->opMode = opMode;
  ncpyOpInfo->refPtr = refPtr;

  return ncpyOpInfo;
}

// Put Methods
void CmiNcpyBuffer::memcpyPut(CmiNcpyBuffer &destination) {
  // memcpy the data from the source buffer into the destination buffer
  memcpy((void *)destination.ptr, ptr, std::min(cnt, destination.cnt));
}

#if CMK_USE_CMA
void CmiNcpyBuffer::cmaPut(CmiNcpyBuffer &destination) {
  CmiIssueRputUsingCMA(destination.ptr, destination.layerInfo, destination.pe,
                       ptr, layerInfo, pe, std::min(cnt, destination.cnt));
}
#endif

void CmiNcpyBuffer::rdmaPut(CmiNcpyBuffer &destination, int ackSize,
                            char *srcAck, char *destAck) {
  registerForTransfer(*this);

  int rootNode = -1; // -1 is the rootNode for p2p operations

  NcpyOperationInfo *ncpyOpInfo =
      createNcpyOpInfo(*this, destination, ackSize, srcAck, destAck, rootNode,
                       CMK_DIRECT_API, (void *)ref);

  CmiIssueRput(ncpyOpInfo);
}

// Returns CmiNcpyMode::MEMCPY if both the PEs are the same and memcpy can be
// used Returns CmiNcpyMode::CMA if both the PEs are in the same physical node
// and CMA can be used Returns CmiNcpyMode::RDMA if RDMA needs to be used
CmiNcpyMode findTransferMode(int srcPe, int destPe) {
  if (CmiNodeOf(srcPe) == CmiNodeOf(destPe))
    return CmiNcpyMode::MEMCPY;
#if CMK_USE_CMA
  else if (useCMAForZC && CmiDoesCMAWork() &&
           CmiPeOnSamePhysicalNode(srcPe, destPe))
    return CmiNcpyMode::CMA;
#endif
  else
    return CmiNcpyMode::RDMA;
}

CmiNcpyMode findTransferModeWithNodes(int srcNode, int destNode) {
  if (srcNode == destNode)
    return CmiNcpyMode::MEMCPY;
#if CMK_USE_CMA
  else if (useCMAForZC && CmiDoesCMAWork() &&
           CmiPeOnSamePhysicalNode(CmiNodeFirst(srcNode),
                                   CmiNodeFirst(destNode)))
    return CmiNcpyMode::CMA;
#endif
  else
    return CmiNcpyMode::RDMA;
}

zcPupSourceInfo *zcPupAddSource(CmiNcpyBuffer &src) {
  zcPupSourceInfo *srcInfo = new zcPupSourceInfo();
  srcInfo->src = src;
  srcInfo->deallocate = free;
  return srcInfo;
}

zcPupSourceInfo *zcPupAddSource(CmiNcpyBuffer &src,
                                std::function<void(void *)> deallocate) {
  zcPupSourceInfo *srcInfo = new zcPupSourceInfo();
  srcInfo->src = src;
  srcInfo->deallocate = deallocate;
  return srcInfo;
}

void zcPupDone(void *ref) {
  zcPupSourceInfo *srcInfo = (zcPupSourceInfo *)(ref);
#if CMK_REG_REQUIRED
  deregisterBuffer(srcInfo->src);
#endif

  srcInfo->deallocate((void *)srcInfo->src.ptr);
  delete srcInfo;
}

void zcPupHandler(ncpyHandlerMsg *msg) { zcPupDone(msg->ref); }

void invokeZCPupHandler(void *ref, int pe) {
  ncpyHandlerMsg *msg = (ncpyHandlerMsg *)CmiAlloc(sizeof(ncpyHandlerMsg));
  msg->ref = (void *)ref;

  CmiSetHandler(msg, zc_pup_handler_idx);
  CmiSyncSendAndFree(pe, sizeof(ncpyHandlerMsg), (char *)msg);
}

void zcPupGet(CmiNcpyBuffer &src, CmiNcpyBuffer &dest) {
  CmiNcpyMode transferMode = findTransferMode(src.pe, dest.pe);
  if (transferMode == CmiNcpyMode::MEMCPY) {
    CmiAbort("zcPupGet: memcpyGet should not happen\n");
  }
#if CMK_USE_CMA
  else if (transferMode == CmiNcpyMode::CMA) {
    dest.cmaGet(src);

#if CMK_REG_REQUIRED
    // De-register destination buffer
    deregisterBuffer(dest);
#endif

    if (src.ref)
      invokeZCPupHandler((void *)src.ref, src.pe);
    else
      CmiAbort("zcPupGet - src.ref is NULL\n");
  }
#endif
  else {
    int ackSize = 0;
    int rootNode = -1; // -1 is the rootNode for p2p operations
    NcpyOperationInfo *ncpyOpInfo = dest.createNcpyOpInfo(
        src, dest, ackSize, NULL, NULL, rootNode, CMK_ZC_PUP, NULL);
    CpvAccess(newZCPupGets).push_back(ncpyOpInfo);
  }
}

// Invoked by the local completion of the Rput operation
void CommRputLocalHandler(comm_backend::Status status) {
  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)status.user_context;
  // Do not narrow ackMode here. A true RDMA put only raises a completion on the
  // initiator, so this handler is responsible for both sides of the operation.
  // The default ackMode set by setNcpyOpInfo is CMK_SRC_DEST_ACK, which lets the
  // upper layer invoke the local ack directly and route the remote one to the
  // peer PE. Narrowing it to CMK_SRC_ACK silently drops the destination ack, so
  // the receiver's callback never runs.
  auto realFreeMe = ncpyOpInfo->freeMe;
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  // Local completion of a put says the source buffer is free again, not that
  // the bytes are visible at the destination. The destination's ack leaves
  // this handler as an ordinary Converse message, and with +ipc a destination
  // in a peer process on this host would receive it through the shared-memory
  // pool, which does not wait for the network: the ack could overtake the
  // data and the receiver's callback would run on a stale buffer. Keeping the
  // ack on the backend puts it behind this completion, the same ordering the
  // network path has always relied on (persist-comm.cpp writeToBuffer avoids
  // the same race by not putting to a pool peer at all).
  //
  // The scope covers the whole handler rather than one send because the ack
  // is raised by the upper layer, not from here: Charm++ reaches the
  // destination through invokeDestinationCallback, or through
  // CmiInvokeRemoteDeregAckHandler when registration is required, and a
  // reconverse program through whatever its own RdmaAckCallerFn sends.
  // Read destPe first: the handler may hand ncpyOpInfo on.
  const bool orderBehindPut = CmiIpcReaches(CmiNodeOf(ncpyOpInfo->destPe));
  if (orderBehindPut)
    CmiIpcBeginNetworkOnly();
  ncpyDirectAckHandlerFn(ncpyOpInfo);
  if (orderBehindPut)
    CmiIpcEndNetworkOnly();
  if (realFreeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);  
}

// Invoked by the local completion of the Rget operation
void CommRgetLocalHandler(comm_backend::Status status) {
  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)status.user_context;
  // See CommRputLocalHandler: leave ackMode at its CMK_SRC_DEST_ACK default so
  // the source ack is not dropped. No ordering scope is needed here: local
  // completion of a get means the data is already in this process, so nothing
  // this handler sends can outrun it.
  auto realFreeMe = ncpyOpInfo->freeMe;
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  ncpyDirectAckHandlerFn(ncpyOpInfo);
  if (realFreeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);
}

// RDMA get from a registered remote source into the local destination.
static void rdmaIssueGet(NcpyOperationInfo *ncpyOpInfo, size_t len,
                         comm_backend::CompHandler done = CommRgetLocalHandler) {
  auto mr = *(comm_backend::mr_t *)ncpyOpInfo->destLayerInfo;
  void *rmr = ncpyOpInfo->srcLayerInfo + sizeof(comm_backend::mr_t);
  comm_backend::issueRget(CmiNodeToGlobal(CmiNodeOf(ncpyOpInfo->srcPe)),
                          ncpyOpInfo->destPtr,
                          len, mr, (void*)ncpyOpInfo->srcPtr, rmr,
                          done, ncpyOpInfo);
}

// RDMA put from the local source into a registered remote destination.
static void rdmaIssuePut(NcpyOperationInfo *ncpyOpInfo, size_t len,
                         comm_backend::CompHandler done = CommRputLocalHandler) {
  auto mr = *(comm_backend::mr_t *)ncpyOpInfo->srcLayerInfo;
  void *rmr = ncpyOpInfo->destLayerInfo + sizeof(comm_backend::mr_t);
  comm_backend::issueRput(CmiNodeToGlobal(CmiNodeOf(ncpyOpInfo->destPe)),
                          ncpyOpInfo->srcPtr,
                          len, mr, 0, rmr,
                          done, ncpyOpInfo);
}

// The remote buffer of an RDMA operation is not registered (CMK_BUFFER_UNREG
// or CMK_BUFFER_NOREG), so its layerInfo holds no memory region. Send the
// NcpyOperationInfo to the buffer's owner, which registers the buffer and
// performs the opposite operation (regAndPutHandler, regAndGetHandler).
static void sendRegRequest(NcpyOperationInfo *ncpyOpInfo, int ownerPe,
                           int handler) {
  int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;
  ConverseRdmaMsg *regMsg =
      (ConverseRdmaMsg *)CmiAlloc(sizeof(ConverseRdmaMsg) + ncpyOpInfoSize);
  memcpy((char *)regMsg + sizeof(ConverseRdmaMsg), (char *)ncpyOpInfo,
         ncpyOpInfoSize);
  if (ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);
  CmiSetHandler(regMsg, handler);
  CmiSyncSendAndFree(ownerPe, sizeof(ConverseRdmaMsg) + ncpyOpInfoSize,
                     regMsg);
}

// The NcpyOperationInfo of a registration request, copied out of the message
// so that it outlives the handler; the completion handler frees it.
static NcpyOperationInfo *takeRegRequest(ConverseRdmaMsg *msg) {
  NcpyOperationInfo *embedded =
      (NcpyOperationInfo *)((char *)msg + sizeof(ConverseRdmaMsg));
  int ncpyOpInfoSize = embedded->ncpyOpInfoSize;
  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyOpInfoSize);
  memcpy((char *)ncpyOpInfo, (char *)embedded, ncpyOpInfoSize);
  CmiFree(msg);
  resetNcpyOpInfoPointers(ncpyOpInfo);
  ncpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO;
  return ncpyOpInfo;
}

// The owner registers the buffer for this one transfer, in the operation's
// copy of layerInfo; the user's handle on the owner stays unregistered, so
// the next transfer from it registers again. With CMK_BUFFER_DEREG the layer
// above releases the registration, as it does for any buffer in that mode.
// CMK_BUFFER_NODEREG leaves a registration for the user to reuse and release,
// but the user never held this one, so the owner releases it when the
// transfer completes here (#331). Clearing isSrcRegistered/isDestRegistered
// tells the acknowledgement, and the handle given to a Charm++ callback, that
// the buffer is no longer registered.
static void releaseOwnerRegistration(NcpyOperationInfo *info, bool source) {
  if (source) {
    if (info->srcDeregMode != CMK_BUFFER_NODEREG)
      return;
    CmiDeregisterMem(info->srcPtr,
                     info->srcLayerInfo + CmiGetRdmaCommonInfoSize(),
                     info->srcPe, info->srcRegMode);
    info->isSrcRegistered = 0;
  } else {
    if (info->destDeregMode != CMK_BUFFER_NODEREG)
      return;
    CmiDeregisterMem(info->destPtr,
                     info->destLayerInfo + CmiGetRdmaCommonInfoSize(),
                     info->destPe, info->destRegMode);
    info->isDestRegistered = 0;
  }
}

// Local completion of the put in regAndPutHandler: the source has been read.
static void regAndPutDone(comm_backend::Status status) {
  releaseOwnerRegistration((NcpyOperationInfo *)status.user_context, true);
  CommRputLocalHandler(status);
}

// Local completion of the get in regAndGetHandler: the data has landed.
static void regAndGetDone(comm_backend::Status status) {
  releaseOwnerRegistration((NcpyOperationInfo *)status.user_context, false);
  CommRgetLocalHandler(status);
}

// On the source of a get whose source was unregistered: register the source
// and put it to the destination. The single ack is raised here.
static void regAndPutHandler(ConverseRdmaMsg *msg) {
  NcpyOperationInfo *ncpyOpInfo = takeRegRequest(msg);
  CmiSetRdmaBufferInfo(ncpyOpInfo->srcLayerInfo + CmiGetRdmaCommonInfoSize(),
                       ncpyOpInfo->srcPtr, ncpyOpInfo->srcSize,
                       ncpyOpInfo->srcRegMode);
  ncpyOpInfo->isSrcRegistered = 1;
  rdmaIssuePut(ncpyOpInfo,
               std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize),
               regAndPutDone);
}

// On the destination of a put whose destination was unregistered: register
// the destination and get from the source. The single ack is raised here.
static void regAndGetHandler(ConverseRdmaMsg *msg) {
  NcpyOperationInfo *ncpyOpInfo = takeRegRequest(msg);
  CmiSetRdmaBufferInfo(ncpyOpInfo->destLayerInfo + CmiGetRdmaCommonInfoSize(),
                       ncpyOpInfo->destPtr, ncpyOpInfo->destSize,
                       ncpyOpInfo->destRegMode);
  ncpyOpInfo->isDestRegistered = 1;
  rdmaIssueGet(ncpyOpInfo,
               std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize),
               regAndGetDone);
}

/* Perform an RDMA Get operation into the local destination address from the
 * remote source address*/
void CmiIssueRget(NcpyOperationInfo *ncpyOpInfo) {
  int target_node = CmiNodeOf(ncpyOpInfo->srcPe);
  // A posted destination buffer may be smaller than the source: the zerocopy
  // post API lets the receiver choose its own length. The transfer is bounded
  // by both sides, so use the minimum for every length here. The copy-based
  // path (putDataHandler) and memcpyGet/memcpyPut already do this, as does
  // classic charm's ofi layer (src/arch/ofi/machine-onesided.C:280,340).
  size_t len = std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize);
  if (target_node == CmiMyNode()) {
    // loopback: use GPU-aware copy if either pointer is a device allocation
    memcpyAnyPtr((void *)ncpyOpInfo->destPtr, ncpyOpInfo->srcPtr, len);
    comm_backend::Status status;
    status.local_buf = ncpyOpInfo->destPtr;
    status.size = len;
    status.user_context = ncpyOpInfo;
    CommRgetLocalHandler(status);
  } else if (!CmiUseCopyBasedRDMA) {
    if (ncpyOpInfo->isSrcRegistered == 0) {
      // The completion runs on the source, so Charm++ sees a reverse operation.
      setReverseModeForNcpyOpInfo(ncpyOpInfo);
      sendRegRequest(ncpyOpInfo, ncpyOpInfo->srcPe, reg_and_put_handler_idx);
    } else {
      rdmaIssueGet(ncpyOpInfo, len);
    }
  } else {
    CmiIssueRgetCopyBased(ncpyOpInfo);
  }
}

/* Perform an RDMA Put operation into the remote destination address from the
 * local source address */
void CmiIssueRput(NcpyOperationInfo *ncpyOpInfo) {
  int target_node = CmiNodeOf(ncpyOpInfo->destPe);
  // See CmiIssueRget: a posted destination may be smaller than the source, so
  // the transfer length is bounded by both sides.
  size_t len = std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize);
  if (target_node == CmiMyNode()) {
    // loopback: use GPU-aware copy if either pointer is a device allocation
    memcpyAnyPtr((void *)ncpyOpInfo->destPtr, ncpyOpInfo->srcPtr, len);
    comm_backend::Status status;
    status.local_buf = ncpyOpInfo->srcPtr;
    status.size = len;
    status.user_context = ncpyOpInfo;
    CommRputLocalHandler(status);
  } else if (!CmiUseCopyBasedRDMA) {
    if (ncpyOpInfo->isDestRegistered == 0)
      sendRegRequest(ncpyOpInfo, ncpyOpInfo->destPe, reg_and_get_handler_idx);
    else
      rdmaIssuePut(ncpyOpInfo, len);
  } else {
    CmiIssueRputCopyBased(ncpyOpInfo);
  }
}

/****************************** Put with signal
 * *****************************/

// The kinds this process registered through CmiRegisterRputSignalKind,
// indexed by the kind's number. That number is the backend's, from
// comm_backend::registerPutSignalKind, so it is the same one the backend's
// signals carry; a kind registered with the backend directly leaves its slot
// here empty. Without the backend's put with signal, there are no backend
// kinds, and kinds are numbered here, from 0 in registration order. An entry
// is published by its pointer before the count that covers it.
namespace {
struct RputSignalKind {
  CmiRputSignalHandler handler;
  void *ctx;
};
constexpr int kMaxRputSignalKinds = 64;
std::atomic<RputSignalKind *> rputSignalKinds[kMaxRputSignalKinds];
std::atomic<int> numRputSignalKinds{0};  // one past the highest kind here

// A put that Converse carries as one message: the data, its destination and,
// for a put with signal (kind >= 0), the signal. The data follows the struct.
struct PutSignalMsg {
  char core[CmiMsgHeaderSizeBytes];
  void *destPtr;
  uint64_t size;
  int destPe;
  int kind;  // -1: CmiRdmaPutRaw, no signal
  int srcNode;
  uint32_t signal;
};
} // namespace

static const RputSignalKind *findRputSignalKind(int kind) {
  if (kind < 0 || kind >= numRputSignalKinds.load(std::memory_order_acquire))
    return nullptr;
  return rputSignalKinds[kind].load(std::memory_order_acquire);
}

static void raiseRputSignal(int kind, int destPe, uint16_t signal, int srcNode,
                            size_t size) {
  const RputSignalKind *k = findRputSignalKind(kind);
  if (k == nullptr)
    CmiAbort("put with signal: kind %d is not registered with "
             "CmiRegisterRputSignalKind in this process\n", kind);
  k->handler(k->ctx, destPe, signal, srcNode, size);
}

// The backend's handler for every kind registered here; ctx is the kind's
// entry.
static void backendRputSignal(void *ctx, int destRank, uint16_t signal,
                              int srcRank, size_t size) {
  const RputSignalKind *k = (const RputSignalKind *)ctx;
  k->handler(k->ctx, CmiNodeFirst(CmiMyNode()) + destRank, signal, srcRank,
             size);
}

int CmiIsPutSignalCapable(void) {
  return !CmiUseCopyBasedRDMA && comm_backend::isPutSignalCapable();
}

int CmiRegisterRputSignalKind(CmiRputSignalHandler handler, void *ctx) {
  if (CmiMyRank() != 0)
    CmiAbort("CmiRegisterRputSignalKind: call it on rank 0 of each process, "
             "not on PE %d (rank %d)\n", CmiMyPe(), CmiMyRank());
  if (handler == nullptr)
    CmiAbort("CmiRegisterRputSignalKind: no handler\n");
  RputSignalKind *entry = new RputSignalKind{handler, ctx};
  int count = numRputSignalKinds.load(std::memory_order_relaxed);
  int kind = CmiIsPutSignalCapable()
                 ? comm_backend::registerPutSignalKind(
                       backendRputSignal, entry, CmiMyNodeSize())
                 : count;
  if (kind < 0 || kind >= kMaxRputSignalKinds)
    CmiAbort("CmiRegisterRputSignalKind: kind %d is outside 0 to %d\n", kind,
             kMaxRputSignalKinds - 1);
  if (rputSignalKinds[kind].load(std::memory_order_relaxed) != nullptr)
    CmiAbort("CmiRegisterRputSignalKind: kind %d is registered twice\n", kind);
  rputSignalKinds[kind].store(entry, std::memory_order_release);
  if (kind >= count)
    numRputSignalKinds.store(kind + 1, std::memory_order_release);
  return kind;
}

// On a PE of the destination's process: copy the data, then raise the signal.
static void putSignalMsgHandler(void *msg) {
  PutSignalMsg *m = (PutSignalMsg *)msg;
  if (m->size > 0)
    memcpyAnyPtr(m->destPtr, (char *)msg + sizeof(PutSignalMsg), m->size);
  if (m->kind >= 0)
    raiseRputSignal(m->kind, m->destPe, (uint16_t)m->signal, m->srcNode,
                    m->size);
  CmiFree(msg);
}

// The put as one message carrying the data: correct on every backend, since
// the data and the signal arrive together. The source has been copied when
// this returns.
static void sendPutSignalMsg(int destPe, void *destPtr, const void *src,
                             size_t size, int kind, uint16_t signal) {
  size_t total = sizeof(PutSignalMsg) + size;
  if (total > (size_t)INT_MAX)
    CmiAbort("put with signal: %zu bytes do not fit one Converse message\n",
             size);
  PutSignalMsg *m = (PutSignalMsg *)CmiAlloc((int)total);
  m->destPtr = destPtr;
  m->size = size;
  m->destPe = destPe;
  m->kind = kind;
  m->srcNode = CmiMyNodeGlobal();
  m->signal = signal;
  memcpyAnyPtr((char *)m + sizeof(PutSignalMsg), src, size);
  CmiSetHandler(m, put_signal_msg_handler_idx);
  CmiSyncSendAndFree(destPe, (int)total, m);
}

void CmiIssueRputSignal(NcpyOperationInfo *ncpyOpInfo, int destPe, int kind,
                        uint16_t signal) {
  int target_node = CmiNodeOf(ncpyOpInfo->destPe);
  if (destPe < 0 || destPe >= CmiNumPes() || CmiNodeOf(destPe) != target_node)
    CmiAbort("CmiIssueRputSignal: PE %d is not in the process of the "
             "destination buffer (PE %d)\n", destPe, ncpyOpInfo->destPe);
  if (findRputSignalKind(kind) == nullptr)
    CmiAbort("CmiIssueRputSignal: kind %d is not registered with "
             "CmiRegisterRputSignalKind\n", kind);
  // See CmiIssueRget: the transfer is bounded by both buffers.
  size_t len = std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize);
  comm_backend::Status status;
  status.local_buf = ncpyOpInfo->srcPtr;
  status.size = len;
  status.user_context = ncpyOpInfo;
  if (target_node == CmiMyNode()) {
    memcpyAnyPtr((void *)ncpyOpInfo->destPtr, ncpyOpInfo->srcPtr, len);
    raiseRputSignal(kind, destPe, signal, CmiMyNodeGlobal(), len);
    CommRputLocalHandler(status);
  } else if (!CmiIsPutSignalCapable()) {
    // Converse's emulation. A put followed by a message after its local
    // completion would not do: LCI reports an injected put complete when it
    // is posted, so the message could overtake the data.
    sendPutSignalMsg(destPe, (void *)ncpyOpInfo->destPtr, ncpyOpInfo->srcPtr,
                     len, kind, signal);
    CommRputLocalHandler(status);
  } else {
    if (!ncpyOpInfo->isSrcRegistered || !ncpyOpInfo->isDestRegistered)
      CmiAbort("CmiIssueRputSignal: the %s buffer is not registered\n",
               ncpyOpInfo->isSrcRegistered ? "destination" : "source");
    auto mr = *(comm_backend::mr_t *)ncpyOpInfo->srcLayerInfo;
    void *rmr = ncpyOpInfo->destLayerInfo + sizeof(comm_backend::mr_t);
    comm_backend::issueRputSignal(CmiNodeToGlobal(target_node),
                                  ncpyOpInfo->srcPtr, len, mr, 0, rmr,
                                  CmiRankOf(destPe), kind, signal,
                                  CommRputLocalHandler, ncpyOpInfo);
  }
}

static void rawPutDone(comm_backend::Status) {}

void CmiRdmaPutRaw(const CmiNcpyBuffer &src, const CmiNcpyBuffer &dest,
                   size_t destOffset) {
  if (destOffset > dest.cnt || src.cnt > dest.cnt - destOffset)
    CmiAbort("CmiRdmaPutRaw: %zu bytes at offset %zu do not fit the %zu-byte "
             "destination\n", src.cnt, destOffset, dest.cnt);
  void *destPtr = (char *)dest.ptr + destOffset;
  int destNode = CmiNodeOf(dest.pe);
  if (destNode == CmiMyNode()) {
    memcpyAnyPtr(destPtr, src.ptr, src.cnt);
  } else if (CmiUseCopyBasedRDMA) {
    sendPutSignalMsg(dest.pe, destPtr, src.ptr, src.cnt, -1, 0);
  } else {
    if (!src.isRegistered || !dest.isRegistered)
      CmiAbort("CmiRdmaPutRaw: the %s buffer is not registered\n",
               src.isRegistered ? "destination" : "source");
    const char *srcInfo = src.layerInfo + CmiGetRdmaCommonInfoSize();
    const char *destInfo = dest.layerInfo + CmiGetRdmaCommonInfoSize();
    auto mr = *(const comm_backend::mr_t *)srcInfo;
    void *rmr = (void *)(destInfo + sizeof(comm_backend::mr_t));
    comm_backend::issueRput(CmiNodeToGlobal(destNode), src.ptr, src.cnt, mr,
                            destOffset, rmr, rawPutDone, nullptr);
  }
}

// Registrations made by CmiSetRdmaBufferInfo and not yet released by
// CmiDeregisterMem, in this process: a buffer may be registered on one PE and
// released on another.
static std::atomic<long> registeredBuffers{0};

long CmiRegisteredBuffers(void) {
  return registeredBuffers.load(std::memory_order_relaxed);
}

/* De-register registered memory for pointer */
void CmiDeregisterMem(const void *ptr, void *info, int pe,
                      unsigned short int mode) {
// #if CMK_USE_LRTS && CMK_ONESIDED_IMPL
//   LrtsDeregisterMem(ptr, info, pe, mode);
// #endif
  if (!CmiUseCopyBasedRDMA) {
    comm_backend::deregisterMemory(*(comm_backend::mr_t *)info);
    registeredBuffers.fetch_sub(1, std::memory_order_relaxed);
  }
}

// FIXME: This really should be implemented in the charm layer
void CmiInvokeRemoteDeregAckHandler(int pe, NcpyOperationInfo *ncpyOpInfo) {
// #if CMK_USE_LRTS && CMK_ONESIDED_IMPL
//   LrtsInvokeRemoteDeregAckHandler(pe, ncpyOpInfo);
// #endif
  if(ncpyOpInfo->opMode == CMK_BCAST_EM_API)
    return;
  if(ncpyOpInfo->opMode != CMK_DIRECT_API && ncpyOpInfo->opMode != CMK_EM_API)
    CmiAbort("CmiInvokeRemoteDeregAckHandler: ncpyOpInfo->opMode is not valid for dereg\n");
  // Whether the caller's NcpyOperationInfo is ours to free is what its freeMe
  // says, not what its opMode implies. Charm++ allocates a Direct API info
  // with CmiAlloc and leaves freeMe at CMK_FREE_NCPYOPINFO for the layer to
  // release; the copy-based handlers above pass infos that live inside a
  // received message, with freeMe cleared. Freeing those by opMode was an
  // interior free of a message buffer ("LCI: mempool lower boundary
  // violation" under +nordma, reconverse #221).
  bool freeInfo = (ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO);

  int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;
  // Send a ConverseRdmaMsg to other PE requesting it to send the array
  ConverseRdmaMsg *remoteDeregMsg =
      (ConverseRdmaMsg *)CmiAlloc(sizeof(ConverseRdmaMsg) + ncpyOpInfoSize);

  // copy the additional Info into the getReqMsg
  memcpy((char *)remoteDeregMsg + sizeof(ConverseRdmaMsg), (char *)ncpyOpInfo,
         ncpyOpInfoSize);

  CmiSetHandler(remoteDeregMsg, remote_dereg_handler_idx);
  CmiSyncSendAndFree(pe,
                     sizeof(ConverseRdmaMsg) + ncpyOpInfoSize, remoteDeregMsg);

  // free original ncpyOpinfo
  if (freeInfo)
    CmiFree(ncpyOpInfo);
}

/* Set the machine specific information for a nocopy pointer */
void CmiSetRdmaBufferInfo(void *info, const void *ptr, int size,
                          unsigned short int mode) {
#if CMK_USE_LRTS && CMK_ONESIDED_IMPL
  LrtsSetRdmaBufferInfo(info, ptr, size, mode);
#endif
  if (!CmiUseCopyBasedRDMA) {
    // register the memory
    // The info buffer is used for both local and remote memory operation
    // We need to write both the local memory region (mr_t) and the remote
    // memory region (rmr) into the info
    auto mr = comm_backend::registerMemory((void *)ptr, size);
    registeredBuffers.fetch_add(1, std::memory_order_relaxed);
    static_assert(CMK_NOCOPY_DIRECT_BYTES >= sizeof(mr),
                  "CMK_NOCOPY_DIRECT_BYTES cannot hold an mr_t");
    memcpy(info, &mr, sizeof(mr));
    info = (char *)info + sizeof(mr);
    size_t info_size_left = CMK_NOCOPY_DIRECT_BYTES - sizeof(mr);
    size_t rmr_size = comm_backend::getRMR(mr, info, info_size_left);
    // getRMR writes nothing when the space is too small, and the peer would
    // then read or write through a garbage region, so this check stays in
    // optimized builds, where CmiAssert is compiled out.
    if (rmr_size > info_size_left)
      CmiAbort("CMK_NOCOPY_DIRECT_BYTES (%d) is too small: the backend's remote "
               "region needs %zu bytes after the %zu-byte mr_t\n",
               CMK_NOCOPY_DIRECT_BYTES, rmr_size, sizeof(mr));
  }
}

/* Set the ack handler function used in the Direct API */
void CmiSetDirectNcpyAckHandler(RdmaAckCallerFn fn) {
  ncpyDirectAckHandlerFn = fn;
}

#if CMK_USE_CMA
#include <unistd.h>
#endif

/* Support for Nocopy Direct API */
typedef struct _cmi_common_rdma_info {
#if CMK_USE_CMA
  pid_t pid;
#elif defined _MSC_VER
  char empty;
#endif
} CmiCommonRdmaInfo_t;

/* Set the generic converse/LRTS information */
void CmiSetRdmaCommonInfo(void *info, const void *ptr, int size) {
#if CMK_USE_CMA
  CmiCommonRdmaInfo_t *cmmInfo = (CmiCommonRdmaInfo_t *)info;
  cmmInfo->pid = getpid();
#endif
}

int CmiGetRdmaCommonInfoSize() {
#if CMK_USE_CMA
  return sizeof(CmiCommonRdmaInfo_t);
#else
  return 0; // If CMK_USE_CMA is false, sizeof(CmiCommonRdmaInfo_t) is 1 (size
            // of an empty structure in C++) However, 0 is returned since
            // CMK_COMMON_NOCOPY_DIRECT_BYTES is set to 0 when CMK_USE_CMA is
            // false because the offset (returned by CmiGetRdmaCommonInfoSize)
            // should equal CMK_COMMON_NOCOPY_DIRECT_BYTES
#endif
}

#if CMK_USE_CMA
#include <sys/uio.h> // for struct iovec
#include <unistd.h>
extern int cma_works;
int readShmCma(pid_t, char *, char *, size_t);
int writeShmCma(pid_t, char *, char *, size_t);

// These methods are also used by the generic layer implementation of the Direct
// API
void CmiIssueRgetUsingCMA(const void *srcAddr, void *srcInfo, int srcPe,
                          const void *destAddr, void *destInfo, int destPe,
                          size_t size) {
  // get remote process id
  CmiCommonRdmaInfo_t *remoteCommInfo = (CmiCommonRdmaInfo_t *)srcInfo;
  pid_t pid = remoteCommInfo->pid;
  readShmCma(pid, (char *)destAddr, (char *)srcAddr, size);
}

void CmiIssueRputUsingCMA(const void *destAddr, void *destInfo, int destPe,
                          const void *srcAddr, void *srcInfo, int srcPe,
                          size_t size) {
  // get remote process id
  CmiCommonRdmaInfo_t *remoteCommInfo = (CmiCommonRdmaInfo_t *)destInfo;
  pid_t pid = remoteCommInfo->pid;
  writeShmCma(pid, (char *)srcAddr, (char *)destAddr, size);
}
#endif

void CmiInvokeNcpyAck(void *ack) { ncpyDirectAckHandlerFn(ack); }
