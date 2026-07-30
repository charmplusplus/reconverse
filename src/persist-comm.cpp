/* Persistent communication support.
 *
 * A persistent channel pins down the destination of a repeated point-to-point
 * send: the receive buffers are allocated (and registered with the comm
 * backend) once at setup time, and every later message reuses them. When the
 * backend supports RMA the payload travels as a one-sided put straight into
 * the destination buffer, followed by a small notification message; otherwise
 * the payload rides along inside the notification and the receiver copies it
 * into the same buffer, so both paths deliver messages identically.
 *
 * A channel whose destination is on this node is a special case: the PEs share
 * an address space, so the ordinary send path already delivers the message by
 * pointer with no copy and no network involved. Such a channel skips setup
 * entirely, allocates no buffers, and forwards to that path.
 *
 * Buffer reuse is credit based. A channel owns PERSIST_BUFFERS_NUM buffers;
 * the sender may only write into a buffer it holds the credit for, and the
 * credit comes back when the receiver releases the delivered message (see
 * CmiPersistentReleaseBuffer). Sends issued while every buffer is taken (or
 * before setup has completed) are queued on the sender and flushed as credits
 * return, so a burst of sends never blocks and never overwrites a buffer that
 * is still in use.
 *
 * All channel bookkeeping lives in per-PE tables that are only touched by the
 * PE that owns them: setup, teardown, notification, and credit all arrive as
 * Converse messages handled on the owning PE.
 */

#include "conv-rdma.h"
#include "converse_internal.h"
#include "persistent.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

CpvDeclare(PersistentHandle *, phs);
CpvDeclare(int, phsSize);
CpvDeclare(int, curphs);

namespace {

/* "PERSIST" -- guards against mistaking an ordinary block for a receive
   buffer if a bogus pointer ever reaches CmiPersistentReleaseBuffer(). */
constexpr CmiUInt8 kBufMagic = 0x50455253495354ULL;

struct PendingMsg {
  void *msg;
  int size;
};

struct PersistentSendsTable {
  int destPE;
  int destNode;
  int sizeMax;
  bool isLocal;   // destination PE shares our address space
  bool useRdma;   // payload goes out as a one-sided put
  bool setupDone; // the destination has handed us its buffers
  PersistentHandle destHandle;
  PersistentBufDesc bufs[PERSIST_BUFFERS_NUM];
  bool busy[PERSIST_BUFFERS_NUM];
  int nextBuf;
  std::deque<PendingMsg> pending;
};

/* Sits immediately in front of the CmiChunkHeader of every receive buffer, so
   a buffer can be traced back to its channel given only the message pointer.
   Layout of a receive buffer:
     [PersistentBufHeader][CmiChunkHeader][message area of sizeMax bytes] */
struct alignas(ALIGN_BYTES) PersistentBufHeader {
  CmiUInt8 magic;
  void *slot; // PersistentReceivesTable *
  int index;
  int srcPE; // filled in on delivery, used to return the credit
  PersistentHandle srcHandle; // sender-side channel to credit
};

struct PersistentReceivesTable {
  int sizeMax;
  char *base[PERSIST_BUFFERS_NUM];
  char *msgPtr[PERSIST_BUFFERS_NUM];
  comm_backend::mr_t mr[PERSIST_BUFFERS_NUM];
};

constexpr size_t kBufHeaderBytes = ALIGN_DEFAULT(sizeof(PersistentBufHeader));
constexpr size_t kMsgOffset = kBufHeaderBytes + sizeof(CmiChunkHeader);

/* ------------------------------- messages ------------------------------- */

struct PersistentRequestMsg {
  char core[CmiMsgHeaderSizeBytes];
  int requestorPE;
  int maxBytes;
  PersistentHandle sourceHandler;
};

struct PersistentReqGrantedMsg {
  char core[CmiMsgHeaderSizeBytes];
  PersistentHandle sourceHandler;
  PersistentHandle destHandler;
  PersistentBufDesc bufs[PERSIST_BUFFERS_NUM];
};

/* Tells the receiver that buffer bufIndex holds a message of size bytes. When
   inlinePayload is set the payload follows this header and still has to be
   copied into the buffer (the fallback for backends without RMA). */
struct PersistentDataMsg {
  char core[CmiMsgHeaderSizeBytes];
  PersistentHandle destHandler;
  PersistentHandle srcHandler;
  int srcPE;
  int bufIndex;
  int size;
  int inlinePayload;
};

struct PersistentCreditMsg {
  char core[CmiMsgHeaderSizeBytes];
  PersistentHandle sourceHandler;
  int bufIndex;
};

struct PersistentDestroyMsg {
  char core[CmiMsgHeaderSizeBytes];
  PersistentHandle destHandler;
};

/* Captured by value when a put is issued: the completion callback may run on a
   different PE's thread, so it must not reach into any channel table. */
struct PutContext {
  PersistentHandle destHandler;
  PersistentHandle srcHandler;
  int destPE;
  int srcPE;
  int bufIndex;
  int size;
  void *msg;
};

/* ------------------------------ per-PE state ----------------------------- */

CpvStaticDeclare(std::vector<PersistentSendsTable *> *, persistentSendsTable);
CpvStaticDeclare(std::vector<PersistentReceivesTable *> *,
                 persistentReceivesTable);
CpvStaticDeclare(int, persistentInited);
CpvStaticDeclare(int, persistentRequestHandlerIdx);
CpvStaticDeclare(int, persistentReqGrantedHandlerIdx);
CpvStaticDeclare(int, persistentDataHandlerIdx);
CpvStaticDeclare(int, persistentCreditHandlerIdx);
CpvStaticDeclare(int, persistentDestroyHandlerIdx);

bool rdmaAvailable() {
  return !CmiUseCopyBasedRDMA && comm_backend::isRMACapable();
}

/* The tables only ever hold live channels, so membership doubles as a validity
   check for handles that arrive from another PE. A channel can be destroyed
   while a message for it is still in flight, and dropping such a message is
   better than following a dangling pointer. */
PersistentSendsTable *lookupSendSlot(PersistentHandle h) {
  if (h == nullptr)
    return nullptr;
  auto *table = CpvAccess(persistentSendsTable);
  for (auto *slot : *table) {
    if (slot == h)
      return slot;
  }
  return nullptr;
}

PersistentReceivesTable *lookupRecvSlot(PersistentHandle h) {
  if (h == nullptr)
    return nullptr;
  auto *table = CpvAccess(persistentReceivesTable);
  for (auto *slot : *table) {
    if (slot == h)
      return slot;
  }
  return nullptr;
}

PersistentBufHeader *bufHeaderOf(void *msg) {
  return (PersistentBufHeader *)((char *)msg - kMsgOffset);
}

PersistentReceivesTable *newRecvSlot(int maxBytes) {
  if (maxBytes <= 0)
    CmiAbort("Persistent communication: maxBytes must be positive\n");

  auto *slot = new PersistentReceivesTable();
  slot->sizeMax = (int)ALIGN_DEFAULT((size_t)maxBytes);

  const size_t total = kMsgOffset + (size_t)slot->sizeMax;
  const bool useRdma = rdmaAvailable();

  for (int i = 0; i < PERSIST_BUFFERS_NUM; i++) {
    void *base = nullptr;
    if (posix_memalign(&base, ALIGN_BYTES, total) != 0 || base == nullptr)
      CmiAbort("Persistent communication: could not allocate a %zu byte "
               "receive buffer\n",
               total);
    memset(base, 0, kMsgOffset);

    char *msgPtr = (char *)base + kMsgOffset;
    auto *hdr = (PersistentBufHeader *)base;
    hdr->magic = kBufMagic;
    hdr->slot = slot;
    hdr->index = i;
    hdr->srcPE = -1;
    hdr->srcHandle = nullptr;

    slot->base[i] = (char *)base;
    slot->msgPtr[i] = msgPtr;
    slot->mr[i] = useRdma ? comm_backend::registerMemory(base, total)
                          : comm_backend::MR_NULL;
    if (useRdma && slot->mr[i] == comm_backend::MR_NULL)
      CmiAbort("Persistent communication: could not register a receive "
               "buffer with the communication backend\n");

    /* Make the message area look like a block handed out by CmiAlloc, so the
       receiving handler can treat a persistent message like any other. The
       reference count sits above CMK_PERSISTENT_REFBASE, which is how CmiFree
       tells the two apart. */
    SIZEFIELD(msgPtr) = slot->sizeMax;
    MRFIELD(msgPtr) = slot->mr[i];
    REFFIELDSET(msgPtr, CMK_PERSISTENT_REFBASE);
  }

  CpvAccess(persistentReceivesTable)->push_back(slot);
  return slot;
}

void fillBufDescs(PersistentReceivesTable *slot, PersistentBufDesc *descs) {
  for (int i = 0; i < PERSIST_BUFFERS_NUM; i++) {
    memset(&descs[i], 0, sizeof(descs[i]));
    descs[i].addr = (CmiUInt8)(uintptr_t)slot->msgPtr[i];
    descs[i].disp = (CmiUInt8)kMsgOffset;
    if (slot->mr[i] != comm_backend::MR_NULL) {
      size_t needed = comm_backend::getRMR(slot->mr[i], descs[i].rmr,
                                           CMK_PERSISTENT_RMR_BYTES);
      /* getRMR writes nothing when the buffer is too small, which would leave
         the sender putting into a null region, so this has to be fatal even
         in an optimized build. */
      if (needed > CMK_PERSISTENT_RMR_BYTES)
        CmiAbort("Persistent communication: CMK_PERSISTENT_RMR_BYTES (%d) is "
                 "too small for this backend, it needs %zu bytes\n",
                 (int)CMK_PERSISTENT_RMR_BYTES, needed);
    }
  }
}

void freeRecvSlot(PersistentReceivesTable *slot) {
  for (int i = 0; i < PERSIST_BUFFERS_NUM; i++) {
    if (slot->mr[i] != comm_backend::MR_NULL)
      comm_backend::deregisterMemory(slot->mr[i]);
    free(slot->base[i]);
  }
  delete slot;
}

PersistentSendsTable *newSendSlot(int destPE, int maxBytes) {
  if (destPE < 0 || destPE >= CmiNumPes())
    CmiAbort("Persistent communication: destination PE %d is out of range\n",
             destPE);

  auto *slot = new PersistentSendsTable();
  slot->destPE = destPE;
  slot->destNode = CmiNodeOf(destPE);
  slot->sizeMax = (int)ALIGN_DEFAULT((size_t)maxBytes);
  slot->isLocal = (slot->destNode == CmiMyNode());
  slot->useRdma = !slot->isLocal && rdmaAvailable();
  slot->setupDone = false;
  slot->destHandle = nullptr;
  slot->nextBuf = 0;
  memset(slot->bufs, 0, sizeof(slot->bufs));
  for (int i = 0; i < PERSIST_BUFFERS_NUM; i++)
    slot->busy[i] = false;

  CpvAccess(persistentSendsTable)->push_back(slot);
  return slot;
}

void freeSendSlot(PersistentSendsTable *slot) {
  /* Anything still queued was never handed to the network, so it is ours to
     release. */
  for (auto &pending : slot->pending)
    CmiFree(pending.msg);
  slot->pending.clear();

  auto *table = CpvAccess(persistentSendsTable);
  for (auto it = table->begin(); it != table->end(); ++it) {
    if (*it == slot) {
      table->erase(it);
      break;
    }
  }
  delete slot;
}

/* --------------------------------- send --------------------------------- */

void sendNotification(PersistentHandle destHandler, PersistentHandle srcHandler,
                      int destPE, int srcPE, int bufIndex, int size,
                      const void *payload) {
  const int msgSize =
      (int)sizeof(PersistentDataMsg) + (payload != nullptr ? size : 0);
  auto *msg = (PersistentDataMsg *)CmiAlloc(msgSize);
  msg->destHandler = destHandler;
  msg->srcHandler = srcHandler;
  msg->srcPE = srcPE;
  msg->bufIndex = bufIndex;
  msg->size = size;
  msg->inlinePayload = (payload != nullptr);
  if (payload != nullptr)
    memcpy((char *)msg + sizeof(PersistentDataMsg), payload, size);

  CmiSetHandler(msg, CpvAccess(persistentDataHandlerIdx));
  ((CmiMessageHeader *)msg)->messageSize = msgSize;
  CmiSyncSendAndFreeNoPersistent(destPE, msgSize, msg);
}

/* Local completion of a put: the payload has left our buffer, so tell the
   receiver it can pick the message up. This may run on any thread that drives
   the backend, so it only uses the values captured in the context. */
void persistentPutDone(comm_backend::Status status) {
  auto *ctx = (PutContext *)status.user_context;
  sendNotification(ctx->destHandler, ctx->srcHandler, ctx->destPE, ctx->srcPE,
                   ctx->bufIndex, ctx->size, nullptr);
  CmiFree(ctx->msg);
  delete ctx;
}

void writeToBuffer(PersistentSendsTable *slot, int bufIndex, int size,
                   void *msg) {
  slot->busy[bufIndex] = true;
  PersistentBufDesc &buf = slot->bufs[bufIndex];

  if (slot->useRdma) {
    auto *ctx = new PutContext{slot->destHandle, slot, slot->destPE, CmiMyPe(),
                               bufIndex,         size, msg};
    /* The notification is only sent once this put completes locally, which is
       what orders it behind the data on the wire. */
    comm_backend::issueRput(slot->destNode, msg, size, MRFIELD(msg),
                            (uintptr_t)buf.disp, buf.rmr, persistentPutDone,
                            ctx);
  } else {
    /* No one-sided support: the notification carries the payload and the
       receiver copies it into the buffer. */
    sendNotification(slot->destHandle, slot, slot->destPE, CmiMyPe(), bufIndex,
                     size, msg);
    CmiFree(msg);
  }
}

int findFreeBuffer(PersistentSendsTable *slot) {
  for (int i = 0; i < PERSIST_BUFFERS_NUM; i++) {
    int idx = (slot->nextBuf + i) % PERSIST_BUFFERS_NUM;
    if (!slot->busy[idx]) {
      slot->nextBuf = (idx + 1) % PERSIST_BUFFERS_NUM;
      return idx;
    }
  }
  return -1;
}

void flushPending(PersistentSendsTable *slot) {
  while (!slot->pending.empty()) {
    int idx = findFreeBuffer(slot);
    if (idx < 0)
      break;
    PendingMsg pending = slot->pending.front();
    slot->pending.pop_front();
    writeToBuffer(slot, idx, pending.size, pending.msg);
  }
}

/* ------------------------------- handlers ------------------------------- */

void persistentRequestHandler(void *env) {
  auto *msg = (PersistentRequestMsg *)env;

  PersistentReceivesTable *slot = newRecvSlot(msg->maxBytes);

  auto *granted =
      (PersistentReqGrantedMsg *)CmiAlloc(sizeof(PersistentReqGrantedMsg));
  granted->sourceHandler = msg->sourceHandler;
  granted->destHandler = slot;
  fillBufDescs(slot, granted->bufs);

  CmiSetHandler(granted, CpvAccess(persistentReqGrantedHandlerIdx));
  ((CmiMessageHeader *)granted)->messageSize = sizeof(PersistentReqGrantedMsg);
  CmiSyncSendAndFreeNoPersistent(msg->requestorPE,
                                 sizeof(PersistentReqGrantedMsg), granted);

  CmiFree(msg);
}

void persistentReqGrantedHandler(void *env) {
  auto *msg = (PersistentReqGrantedMsg *)env;

  PersistentSendsTable *slot = lookupSendSlot(msg->sourceHandler);
  if (slot != nullptr) {
    slot->destHandle = msg->destHandler;
    memcpy(slot->bufs, msg->bufs, sizeof(slot->bufs));
    slot->setupDone = true;
    flushPending(slot);
  }

  CmiFree(msg);
}

void persistentDataHandler(void *env) {
  auto *msg = (PersistentDataMsg *)env;

  PersistentReceivesTable *slot = lookupRecvSlot(msg->destHandler);
  if (slot == nullptr) {
    /* The channel was destroyed while this message was on its way. */
    CmiFree(msg);
    return;
  }

  if (msg->bufIndex < 0 || msg->bufIndex >= PERSIST_BUFFERS_NUM)
    CmiAbort("Persistent communication: notification names buffer %d of %d\n",
             msg->bufIndex, PERSIST_BUFFERS_NUM);
  if (msg->size < 0 || msg->size > slot->sizeMax)
    CmiAbort("Persistent communication: notification of %d bytes for a "
             "channel of %d bytes\n",
             msg->size, slot->sizeMax);
  char *buf = slot->msgPtr[msg->bufIndex];

  if (msg->inlinePayload)
    memcpy(buf, (char *)msg + sizeof(PersistentDataMsg), msg->size);

  /* Remember who to credit, then hand the buffer over as an ordinary message.
     The credit goes back when the handler releases it. */
  PersistentBufHeader *hdr = bufHeaderOf(buf);
  hdr->srcPE = msg->srcPE;
  hdr->srcHandle = msg->srcHandler;
  SIZEFIELD(buf) = msg->size;
  REFFIELDSET(buf, CMK_PERSISTENT_REFBASE + 1);

  CmiFree(msg);
  CmiHandleMessage(buf);
}

void persistentCreditHandler(void *env) {
  auto *msg = (PersistentCreditMsg *)env;

  PersistentSendsTable *slot = lookupSendSlot(msg->sourceHandler);
  if (slot != nullptr) {
    if (msg->bufIndex < 0 || msg->bufIndex >= PERSIST_BUFFERS_NUM)
      CmiAbort("Persistent communication: credit names buffer %d of %d\n",
               msg->bufIndex, PERSIST_BUFFERS_NUM);
    slot->busy[msg->bufIndex] = false;
    flushPending(slot);
  }

  CmiFree(msg);
}

void persistentDestroyHandler(void *env) {
  auto *msg = (PersistentDestroyMsg *)env;

  PersistentReceivesTable *slot = lookupRecvSlot(msg->destHandler);
  if (slot != nullptr) {
    auto *table = CpvAccess(persistentReceivesTable);
    for (auto it = table->begin(); it != table->end(); ++it) {
      if (*it == slot) {
        table->erase(it);
        break;
      }
    }
    freeRecvSlot(slot);
  }

  CmiFree(msg);
}

} // namespace

/* ------------------------------ public API ------------------------------ */

void CmiPersistentInit(void) {
  CpvInitialize(int, persistentInited);
  if (CpvAccess(persistentInited))
    return;
  CpvAccess(persistentInited) = 1;

  CpvInitialize(PersistentHandle *, phs);
  CpvInitialize(int, phsSize);
  CpvInitialize(int, curphs);
  CpvAccess(phs) = nullptr;
  CpvAccess(phsSize) = 0;
  CpvAccess(curphs) = 0;

  CpvInitialize(std::vector<PersistentSendsTable *> *, persistentSendsTable);
  CpvAccess(persistentSendsTable) = new std::vector<PersistentSendsTable *>();
  CpvInitialize(std::vector<PersistentReceivesTable *> *,
                persistentReceivesTable);
  CpvAccess(persistentReceivesTable) =
      new std::vector<PersistentReceivesTable *>();

  CpvInitialize(int, persistentRequestHandlerIdx);
  CpvInitialize(int, persistentReqGrantedHandlerIdx);
  CpvInitialize(int, persistentDataHandlerIdx);
  CpvInitialize(int, persistentCreditHandlerIdx);
  CpvInitialize(int, persistentDestroyHandlerIdx);

  CpvAccess(persistentRequestHandlerIdx) =
      CmiRegisterHandler((CmiHandler)persistentRequestHandler);
  CpvAccess(persistentReqGrantedHandlerIdx) =
      CmiRegisterHandler((CmiHandler)persistentReqGrantedHandler);
  CpvAccess(persistentDataHandlerIdx) =
      CmiRegisterHandler((CmiHandler)persistentDataHandler);
  CpvAccess(persistentCreditHandlerIdx) =
      CmiRegisterHandler((CmiHandler)persistentCreditHandler);
  CpvAccess(persistentDestroyHandlerIdx) =
      CmiRegisterHandler((CmiHandler)persistentDestroyHandler);
}

PersistentHandle CmiCreatePersistent(int destPE, int maxBytes) {
  PersistentSendsTable *slot = newSendSlot(destPE, maxBytes);

  /* A channel that stays on this node sends by pointer and never touches a
     receive buffer, so there is nothing to set up: no round trip, and no
     buffers allocated on the destination. The handle is usable right away. */
  if (slot->isLocal) {
    slot->setupDone = true;
    return slot;
  }

  auto *msg = (PersistentRequestMsg *)CmiAlloc(sizeof(PersistentRequestMsg));
  msg->requestorPE = CmiMyPe();
  msg->maxBytes = slot->sizeMax;
  msg->sourceHandler = slot;

  CmiSetHandler(msg, CpvAccess(persistentRequestHandlerIdx));
  ((CmiMessageHeader *)msg)->messageSize = sizeof(PersistentRequestMsg);
  CmiSyncSendAndFreeNoPersistent(destPE, sizeof(PersistentRequestMsg), msg);

  return slot;
}

PersistentHandle CmiCreateNodePersistent(int destNode, int maxBytes) {
  /* Any PE of the destination node can host the buffers. */
  return CmiCreatePersistent(CmiNodeFirst(destNode), maxBytes);
}

PersistentReq CmiCreateReceiverPersistent(int maxBytes) {
  PersistentReceivesTable *slot = newRecvSlot(maxBytes);

  PersistentReq req;
  memset(&req, 0, sizeof(req));
  req.pe = CmiMyPe();
  req.maxBytes = slot->sizeMax;
  req.myHand = slot;
  fillBufDescs(slot, req.bufs);
  return req;
}

PersistentHandle CmiRegisterReceivePersistent(PersistentReq req) {
  PersistentSendsTable *slot = newSendSlot(req.pe, req.maxBytes);
  slot->destHandle = req.myHand;
  memcpy(slot->bufs, req.bufs, sizeof(slot->bufs));
  slot->setupDone = true;
  return slot;
}

void CmiSendPersistentMsg(PersistentHandle h, int messageSize, void *msg) {
  auto *slot = (PersistentSendsTable *)h;
  if (slot == nullptr)
    CmiAbort("CmiSendPersistentMsg: null PersistentHandle\n");

  if (messageSize > slot->sizeMax)
    CmiAbort("CmiSendPersistentMsg: message of %d bytes does not fit in a "
             "persistent channel of %d bytes\n",
             messageSize, slot->sizeMax);

  /* A destination on this node shares our address space, so the ordinary send
     path already hands the message over by pointer. Nothing a persistent
     channel can do beats that: staging the payload through a receive buffer
     would only add a copy. The receiving handler cannot tell the difference,
     since releasing a persistent message and freeing an ordinary one are the
     same call. */
  if (slot->isLocal) {
    CmiSyncSendAndFreeNoPersistent(slot->destPE, messageSize, msg);
    return;
  }

  /* Wait for the channel to be set up, or for a buffer to come free, rather
     than blocking the sender. Ordering within the channel is preserved
     because the queue is drained in order. */
  if (!slot->setupDone || !slot->pending.empty()) {
    slot->pending.push_back({msg, messageSize});
    if (slot->setupDone)
      flushPending(slot);
    return;
  }

  int idx = findFreeBuffer(slot);
  if (idx < 0) {
    slot->pending.push_back({msg, messageSize});
    return;
  }
  writeToBuffer(slot, idx, messageSize, msg);
}

void CmiUsePersistentHandle(PersistentHandle *p, int n) {
  if (p == nullptr || n <= 0 || (n == 1 && *p == nullptr)) {
    CpvAccess(phs) = nullptr;
    CpvAccess(phsSize) = 0;
    CpvAccess(curphs) = 0;
    return;
  }
  CpvAccess(phs) = p;
  CpvAccess(phsSize) = n;
  CpvAccess(curphs) = 0;
}

void CmiPersistentOneSend(void) {
  if (CpvAccess(phs) != nullptr)
    CpvAccess(curphs) = (CpvAccess(curphs) + 1) % CpvAccess(phsSize);
}

int CmiPersistentHandleSend(int destPE, int messageSize, void *msg) {
  PersistentHandle h = CpvAccess(phs)[CpvAccess(curphs)];
  /* Walk the array one entry per send; a single handle is simply reused. */
  CmiPersistentOneSend();

  PersistentSendsTable *slot = lookupSendSlot(h);
  if (slot == nullptr)
    return 0; // no channel, or one that has already been destroyed

  if (slot->destPE != destPE) {
    /* The handle was set up for someone else, so it cannot carry this
       message. Fall back to an ordinary send. */
    return 0;
  }

  CmiSendPersistentMsg(h, messageSize, msg);
  return 1;
}

void CmiPersistentReleaseBuffer(void *msg) {
  PersistentBufHeader *hdr = bufHeaderOf(msg);
  if (hdr->magic != kBufMagic)
    CmiAbort("CmiFree: block looks like a persistent receive buffer but has "
             "no valid header\n");

  int srcPE = hdr->srcPE;
  PersistentHandle srcHandle = hdr->srcHandle;
  int index = hdr->index;
  hdr->srcPE = -1;
  hdr->srcHandle = nullptr;

  if (srcHandle == nullptr)
    return; // never delivered, nothing to credit

  auto *credit = (PersistentCreditMsg *)CmiAlloc(sizeof(PersistentCreditMsg));
  credit->sourceHandler = srcHandle;
  credit->bufIndex = index;
  CmiSetHandler(credit, CpvAccess(persistentCreditHandlerIdx));
  ((CmiMessageHeader *)credit)->messageSize = sizeof(PersistentCreditMsg);
  CmiSyncSendAndFreeNoPersistent(srcPE, sizeof(PersistentCreditMsg), credit);
}

void CmiDestroyPersistent(PersistentHandle h) {
  if (h == nullptr)
    return;

  PersistentSendsTable *slot = lookupSendSlot(h);
  if (slot == nullptr)
    return;

  if (slot->destHandle != nullptr) {
    auto *msg = (PersistentDestroyMsg *)CmiAlloc(sizeof(PersistentDestroyMsg));
    msg->destHandler = slot->destHandle;
    CmiSetHandler(msg, CpvAccess(persistentDestroyHandlerIdx));
    ((CmiMessageHeader *)msg)->messageSize = sizeof(PersistentDestroyMsg);
    CmiSyncSendAndFreeNoPersistent(slot->destPE, sizeof(PersistentDestroyMsg),
                                   msg);
  }

  freeSendSlot(slot);
}

void CmiDestroyAllPersistent(void) {
  if (!CpvInitialized(persistentInited) || !CpvAccess(persistentInited))
    return;

  auto sends = *CpvAccess(persistentSendsTable);
  for (auto *slot : sends)
    CmiDestroyPersistent(slot);
  CpvAccess(persistentSendsTable)->clear();

  auto receives = *CpvAccess(persistentReceivesTable);
  CpvAccess(persistentReceivesTable)->clear();
  for (auto *slot : receives)
    freeRecvSlot(slot);

  CmiUsePersistentHandle(nullptr, 0);
}
