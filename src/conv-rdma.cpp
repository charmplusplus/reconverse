/* Support for Direct Nocopy API (Generic Implementation)
 * Specific implementations are in arch/layer/machine-onesided.{h,c}
 */
#include "conv-rdma.h"
#include "converse_internal.h"
#include <algorithm>
#include <cstring>
#include <vector>

// User specified configuration
// TODO: move to a better location
bool CmiUseCopyBasedRDMA = false;

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

// Invoked when this PE has to deregister the local memory
static void remoteDeregHandler(ConverseRdmaMsg *deregMsg) {
  NcpyOperationInfo *ncpyOpInfo = reinterpret_cast<NcpyOperationInfo *>(
      reinterpret_cast<char *>(deregMsg) + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;

  if (CmiMyPe() == ncpyOpInfo->srcPe) {
    ncpyOpInfo->ackMode = CMK_SRC_ACK;
    CmiDeregisterMem(ncpyOpInfo->srcPtr,
                     ncpyOpInfo->srcLayerInfo + CmiGetRdmaCommonInfoSize(),
                     ncpyOpInfo->srcPe, ncpyOpInfo->srcDeregMode);
  } else if (CmiMyPe() == ncpyOpInfo->destPe) {
    ncpyOpInfo->ackMode = CMK_DEST_ACK;
    CmiDeregisterMem(ncpyOpInfo->destPtr,
                     ncpyOpInfo->destLayerInfo + CmiGetRdmaCommonInfoSize(),
                     ncpyOpInfo->destPe, ncpyOpInfo->destDeregMode);
  } else {
    CmiAbort("remoteDeregHandler: Invalid PE\n");
  }

  ncpyDirectAckHandlerFn(ncpyOpInfo);
}

// Invoked when this PE has to send a large array for an Rget
static void getRequestHandler(ConverseRdmaMsg *getReqMsg) {
  NcpyOperationInfo *ncpyOpInfo =
      (NcpyOperationInfo *)((char *)(getReqMsg) + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;

  // Get is implemented internally using a call to Put
  CmiIssueRput(ncpyOpInfo);
}

// Invoked when this PE receives a large array as the target of an Rput or the
// initiator of an Rget
static void putDataHandler(ConverseRdmaMsg *payloadMsg) {
  NcpyOperationInfo *ncpyOpInfo =
      (NcpyOperationInfo *)((char *)payloadMsg + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  // copy the received messsage into the user's destination address
  memcpy((char *)ncpyOpInfo->destPtr,
         (char *)payloadMsg + sizeof(ConverseRdmaMsg) +
             ncpyOpInfo->ncpyOpInfoSize,
         std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize));

  // Invoke the destination ack
  ncpyOpInfo->ackMode = CMK_DEST_ACK; // Only invoke the destination ack
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  ncpyDirectAckHandlerFn(ncpyOpInfo);

  CmiFree(payloadMsg);
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

  // Invoke the source ack
  ncpyOpInfo->ackMode = CMK_SRC_ACK; // only invoke the source ack
  // We need to ensure consistent behavior no matter what ncpyDirectAckHandlerFn actually does
  // so we cannot rely on the charm layer to free the ncpyOpInfo
  auto realFreeMe = ncpyOpInfo->freeMe;
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  ncpyDirectAckHandlerFn(ncpyOpInfo);
  if (realFreeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);

  CmiSetHandler(payloadMsg, put_data_handler_idx); // putDataHandler
  CmiSyncSendAndFree(destPe, sizeof(ConverseRdmaMsg) + ncpyOpInfoSize + size,
                     payloadMsg);
}

void CommRputRemoteHandler(comm_backend::Status status);
void CommRgetRemoteHandler(comm_backend::Status status);

// Rget/Rput operations are implemented as normal converse messages
// This method is invoked during converse initialization to initialize these
// message handlers
void CmiOnesidedDirectInit(void) {
  remote_dereg_handler_idx = CmiRegisterHandler((CmiHandler)remoteDeregHandler);
  get_request_handler_idx = CmiRegisterHandler((CmiHandler)getRequestHandler);
  put_data_handler_idx = CmiRegisterHandler((CmiHandler)putDataHandler);
  zc_pup_handler_idx = CmiRegisterHandler((CmiHandler)zcPupHandler);
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

void CmiNcpyBuffer::rdmaGet(CmiNcpyBuffer &source, int ackSize, char *srcAck,
                            char *destAck) {
  // int ackSize = sizeof(CmiCallback);
  if (regMode == CMK_BUFFER_UNREG) {
    // register it because it is required for RGET
    CmiSetRdmaBufferInfo(layerInfo + CmiGetRdmaCommonInfoSize(), ptr, cnt,
                         regMode);

    isRegistered = true;
  }

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
                destination.ref, rootNode, ncpyOpInfo);

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
  if (regMode == CMK_BUFFER_UNREG) {
    // register it because it is required for RPUT
    CmiSetRdmaBufferInfo(layerInfo + CmiGetRdmaCommonInfoSize(), ptr, cnt,
                         regMode);

    isRegistered = true;
  }

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
  ncpyOpInfo->ackMode = CMK_SRC_ACK;
  auto realFreeMe = ncpyOpInfo->freeMe;
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  ncpyDirectAckHandlerFn(ncpyOpInfo);
  if (realFreeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);  
}

// Invoked by the local completion of the Rget operation
void CommRgetLocalHandler(comm_backend::Status status) {
  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)status.user_context;
  ncpyOpInfo->ackMode = CMK_DEST_ACK;
  auto realFreeMe = ncpyOpInfo->freeMe;
  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;
  ncpyDirectAckHandlerFn(ncpyOpInfo);
  if (realFreeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);
}

/* Perform an RDMA Get operation into the local destination address from the
 * remote source address*/
void CmiIssueRget(NcpyOperationInfo *ncpyOpInfo) {
// #if CMK_USE_LRTS && CMK_ONESIDED_IMPL
//   // Use network RDMA for a PE on a remote host
//   LrtsIssueRget(ncpyOpInfo);
// #else
//   CmiIssueRgetCopyBased(ncpyOpInfo);
// #endif
  int target_node = CmiNodeOf(ncpyOpInfo->srcPe);
  if (target_node == CmiMyNode()) {
    // loopback messages
    memcpy((void *)ncpyOpInfo->destPtr, ncpyOpInfo->srcPtr,
           ncpyOpInfo->srcSize);
    comm_backend::Status status;
    status.local_buf = ncpyOpInfo->destPtr;
    status.size = ncpyOpInfo->srcSize;
    status.user_context = ncpyOpInfo;
    CommRgetLocalHandler(status);
  } else if (!CmiUseCopyBasedRDMA) {
    auto mr = *(comm_backend::mr_t *)ncpyOpInfo->destLayerInfo;
    void *rmr = ncpyOpInfo->srcLayerInfo + sizeof(comm_backend::mr_t);
    // FIXME: we assume the offset to the registered base address is 0 here
    comm_backend::issueRget(CmiNodeOf(ncpyOpInfo->srcPe), ncpyOpInfo->destPtr,
                            ncpyOpInfo->srcSize, mr, 0, rmr,
                            CommRgetLocalHandler, ncpyOpInfo);
  } else {
    CmiIssueRgetCopyBased(ncpyOpInfo);
  }
}

/* Perform an RDMA Put operation into the remote destination address from the
 * local source address */
void CmiIssueRput(NcpyOperationInfo *ncpyOpInfo) {
// #if CMK_USE_LRTS && CMK_ONESIDED_IMPL
//   // Use network RDMA for a PE on a remote host
//   LrtsIssueRput(ncpyOpInfo);
// #else
//   CmiIssueRputCopyBased(ncpyOpInfo);
// #endif
int target_node = CmiNodeOf(ncpyOpInfo->destPe);
if (target_node == CmiMyNode()) {
  // loopback messages
  memcpy((void *)ncpyOpInfo->destPtr, ncpyOpInfo->srcPtr, ncpyOpInfo->srcSize);
  comm_backend::Status status;
  status.local_buf = ncpyOpInfo->srcPtr;
  status.size = ncpyOpInfo->srcSize;
  status.user_context = ncpyOpInfo;
  CommRputLocalHandler(status);
} else if (!CmiUseCopyBasedRDMA) {
  auto mr = *(comm_backend::mr_t *)ncpyOpInfo->srcLayerInfo;
  void *rmr = ncpyOpInfo->destLayerInfo + sizeof(comm_backend::mr_t);
  // FIXME: we assume the offset to the registered base address is 0 here
  comm_backend::issueRput(CmiNodeOf(ncpyOpInfo->destPe), ncpyOpInfo->srcPtr,
                          ncpyOpInfo->srcSize, mr, 0, rmr, CommRputLocalHandler,
                          ncpyOpInfo);
} else {
  CmiIssueRputCopyBased(ncpyOpInfo);
}
}

/* De-register registered memory for pointer */
void CmiDeregisterMem(const void *ptr, void *info, int pe,
                      unsigned short int mode) {
// #if CMK_USE_LRTS && CMK_ONESIDED_IMPL
//   LrtsDeregisterMem(ptr, info, pe, mode);
// #endif
  if (!CmiUseCopyBasedRDMA) {
    comm_backend::deregisterMemory(*(comm_backend::mr_t *)info);
  }
}

// FIXME: This really should be implemented in the charm layer
void CmiInvokeRemoteDeregAckHandler(int pe, NcpyOperationInfo *ncpyOpInfo) {
// #if CMK_USE_LRTS && CMK_ONESIDED_IMPL
//   LrtsInvokeRemoteDeregAckHandler(pe, ncpyOpInfo);
// #endif
  if(ncpyOpInfo->opMode == CMK_BCAST_EM_API)
    return;
  bool freeInfo;
  if(ncpyOpInfo->opMode == CMK_DIRECT_API) {
    freeInfo = true;
  } else if(ncpyOpInfo->opMode == CMK_EM_API) {
    freeInfo = false;
  } else {
    CmiAbort("CmiInvokeRemoteDeregAckHandler: ncpyOpInfo->opMode is not valid for dereg\n");
  }

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
    CmiAssert(CMK_NOCOPY_DIRECT_BYTES >= sizeof(mr));
    memcpy(info, &mr, sizeof(mr));
    info = (char *)info + sizeof(mr);
    size_t info_size_left = CMK_NOCOPY_DIRECT_BYTES - sizeof(mr);
    size_t rmr_size = comm_backend::getRMR(mr, info, info_size_left);
    CmiAssertMsg(rmr_size <= info_size_left, "CMK_NOCOPY_DIRECT_BYTES is too small");
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
