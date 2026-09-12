/* runtime_modes: the optional per-node runtime paths, checked together.
 *
 * Three pieces of reconverse are selected by a flag or a build option rather
 * than exercised by every program, so no other test reaches them:
 *
 *   1. CPU affinity   (+setcpuaffinity / +pemap, RECONVERSE_ENABLE_CPU_AFFINITY)
 *   2. Shared-memory IPC blocks between processes on one host (CMK_USE_SHMEM)
 *   3. The copy-based zerocopy Direct API (+nordma, or a backend without RMA),
 *      including the deregistration round trip Charm++ drives after every
 *      Direct API completion.
 *
 * Charm++ is the caller of record for all three, so each phase does what
 * Charm++'s init.C and ck.C do, in the order they do it, and checks the
 * contracts those callers rely on. The program needs exactly two processes
 * (any PE count that divides evenly) on the same host:
 *
 *   <launcher> -n 2 ./reconverse_runtime_modes +pe 4 +nordma +pemap L0-3
 *
 * Phase 1, affinity. Every PE calls CmiInitCPUAffinity and CmiInitCPUTopology
 * itself, as Charm++ does (reconverse's ConverseInit calls neither), then
 * reports its thread's CPU mask to PE 0. With -expect-bound (passed by the
 * ctest entry on Linux, where binding is implemented) PE 0 requires every mask
 * to hold exactly one CPU and PEs sharing a physical node to hold different
 * ones. Elsewhere the flags must at least be accepted and the run complete.
 *
 * Phase 2, IPC (only when built with CMK_USE_SHMEM). Every PE runs the
 * bootstrap Charm++ runs: CmiIpcInit, then CmiMakeIpcManager from a
 * suspendable thread that sleeps until the segments of all processes on the
 * host are attached. Each PE then sends messages of several sizes to the PE of
 * the same rank in the other process through CmiMsgToIpcBlock and
 * CmiPushIpcBlock, exactly as ck.C's _tryIpcSend does. The receiver checks that
 * the scheduler's CmiPopIpcBlock poll delivered each block to the right PE with
 * the right bytes, and frees it with CmiFree, which must return it to the
 * segment's free list rather than to the heap.
 *
 * Phase 3, Direct API with deregistration. The last PE of process 0 rdmaGets a
 * buffer from the last PE of process 1, then rdmaPuts a different pattern
 * back, for several sizes. The acknowledgement handler does what Charm++'s
 * CkRdmaDirectAckHandler does: in the initiator's process it expects exactly
 * one acknowledgement per operation, with opMode CMK_DIRECT_API and ackMode
 * CMK_SRC_DEST_ACK, and answers it with CmiInvokeRemoteDeregAckHandler for the
 * other side's buffer. (On the copy-based path that acknowledgement runs on
 * the initiating PE; on the RMA path it runs on whichever PE of that process
 * drives network progress, so the initiator-side counters are per process.) On the other side it expects exactly one
 * acknowledgement per operation in return, with opMode
 * CMK_EM_API_SRC_ACK_INVOKE or CMK_EM_API_DEST_ACK_INVOKE, freeMe cleared, and
 * the buffer marked deregistered. Charm++ pairs a QdCreate with each dereg
 * request and a QdProcess with each returned acknowledgement, so the counts on
 * the two sides must match at the end or quiescence detection would hang
 * (reconverse #222); and the acknowledgement for a put must not arrive before
 * the bytes have landed, or the destination's callback would read stale data
 * (reconverse #221).
 */
#include "conv-rdma.h"
#include "converse_config.h" // CMK_USE_SHMEM; converse.h does not expose it
#include <atomic>
#include <converse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// ------------------------------------------------------------- messages ---

struct PlainMsg {
  CmiMessageHeader header;
};

struct AffinityMsg {
  CmiMessageHeader header;
  int pe;
  int physNode;
  int cpuCount; // CPUs in the thread's affinity mask; -1 if unknown
  int firstCpu; // lowest CPU in the mask; -1 if unknown
};

struct IpcMsg {
  CmiMessageHeader header;
  int fromPe;
  int toPe;
  size_t payloadLen;
  // payloadLen pattern bytes follow
};

struct SizeMsg {
  CmiMessageHeader header;
  size_t len;
  int phase; // PHASE_GET or PHASE_PUT: what the buffer is for
};

struct BufMsg {
  CmiMessageHeader header;
  CmiNcpyBuffer buf;
};

struct CountMsg {
  CmiMessageHeader header;
  int count;
};

template <typename T> static T *newMsg(int handlerIdx, size_t extra = 0) {
  T *msg = (T *)CmiAlloc(sizeof(T) + extra);
  CmiSetHandler(msg, handlerIdx);
  return msg;
}

static void sendPlain(int destPe, int handlerIdx) {
  CmiSyncSendAndFree(destPe, sizeof(PlainMsg), newMsg<PlainMsg>(handlerIdx));
}

// ------------------------------------------------------------- patterns ---

static unsigned char patternByte(size_t i, unsigned int seed) {
  return (unsigned char)((i * 131u + seed * 17u + 7u) & 0xffu);
}

static void fillPattern(char *buf, size_t n, unsigned int seed) {
  for (size_t i = 0; i < n; ++i)
    buf[i] = (char)patternByte(i, seed);
}

static void checkPattern(const char *what, const char *buf, size_t n,
                         unsigned int seed) {
  for (size_t i = 0; i < n; ++i) {
    if ((unsigned char)buf[i] != patternByte(i, seed))
      CmiAbort("PE %d: %s, %zu bytes: byte %zu is 0x%02x, expected 0x%02x\n",
               CmiMyPe(), what, n, i, (unsigned char)buf[i],
               patternByte(i, seed));
  }
}

// ---------------------------------------------------------------- state ---

CpvDeclare(int, expectBound); // -expect-bound given

CpvDeclare(int, affinityHIdx);
CpvDeclare(int, affinityReports); // PE 0: reports received
CpvDeclare(AffinityMsg *, affinityTable); // PE 0: one entry per PE

CpvDeclare(int, ipcStartHIdx);
CpvDeclare(int, ipcRecvHIdx);
CpvDeclare(int, ipcDoneHIdx);
CpvDeclare(int, ipcReceived); // this PE: IPC messages delivered so far
CpvDeclare(int, ipcDoneReports); // PE 0

CpvDeclare(int, directStartHIdx);
CpvDeclare(int, prepareHIdx);
CpvDeclare(int, bufReadyHIdx);
CpvDeclare(int, initiatorDoneHIdx);
CpvDeclare(int, peerDoneHIdx);
CpvDeclare(int, directFinishedHIdx);
CpvDeclare(int, exitHIdx);

enum Phase { PHASE_IDLE, PHASE_GET, PHASE_PUT };
CpvDeclare(int, phase);
CpvDeclare(int, sizeIdx);
CpvDeclare(int, signals); // initiator: own ack + peer done, per phase
// Initiator-side state touched by the acknowledgement handler, which on the
// RMA path may run on another PE of the initiator's process.
static std::atomic<int> directAcks{0};    // CMK_DIRECT_API acks this phase
static std::atomic<int> deregRequests{0}; // dereg round trips started
CpvDeclare(int, remoteAcks);      // peer: *_ACK_INVOKE acks this phase
CpvDeclare(int, remoteAcksTotal); // peer: over the whole run
CpvDeclare(int, peerDeregistered); // peer: the layer deregistered localBuf
CpvDeclare(char *, bufBase);
CpvDeclare(size_t, bufLen);
CpvDeclare(CmiNcpyBuffer, localBuf);
CpvDeclare(CmiNcpyBuffer, remoteBuf);

static const size_t ipcSizes[] = {1, 100, 1000, 30000};
static const int numIpcSizes = sizeof(ipcSizes) / sizeof(ipcSizes[0]);

static const size_t directSizes[] = {1, 4095, 65536};
static const int numDirectSizes = sizeof(directSizes) / sizeof(directSizes[0]);

#define TARGET_SEED 11u // peer's buffer before the get
#define POISON_SEED 47u // initiator's buffer before the get
#define SOURCE_SEED 83u // initiator's buffer before the put

// Process 0's last PE initiates; process 1's last PE is the other side. Using
// the last PEs means that with more than one PE per process the operation runs
// on a rank other than 0, so the rank-versus-node comparisons in the layer
// (CmiNodeOf, CmiRankOf) are exercised rather than trivially satisfied.
static int initiatorPe() { return CmiNodeSize(0) - 1; }
static int peerPe() { return CmiNumPes() - 1; }
// The PE with my rank in the other process.
static int ipcPartnerPe() {
  return CmiNodeFirst(1 - CmiMyNode()) + CmiMyRank();
}

// ======================================================= phase 1: affinity ==

static void reportAffinity() {
  AffinityMsg *msg = newMsg<AffinityMsg>(CpvAccess(affinityHIdx));
  msg->pe = CmiMyPe();
  msg->physNode = CmiPhysicalNodeID(CmiMyPe());
  msg->cpuCount = -1;
  msg->firstCpu = -1;
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
    msg->cpuCount = CPU_COUNT(&set);
    for (int c = 0; c < CPU_SETSIZE; ++c) {
      if (CPU_ISSET(c, &set)) {
        msg->firstCpu = c;
        break;
      }
    }
  }
#endif
  CmiSyncSendAndFree(0, sizeof(AffinityMsg), msg);
}

static void startIpcPhase();

// PE 0: collect every PE's mask, judge, and move on.
static void affinityHandler(void *vmsg) {
  AffinityMsg *msg = (AffinityMsg *)vmsg;
  CpvAccess(affinityTable)[msg->pe] = *msg;
  CmiFree(msg);
  if (++CpvAccess(affinityReports) < CmiNumPes())
    return;

  AffinityMsg *table = CpvAccess(affinityTable);
  for (int pe = 0; pe < CmiNumPes(); ++pe) {
    if (table[pe].cpuCount < 0)
      CmiPrintf("PE %d: physical node %d, affinity mask not readable on this "
                "platform\n",
                pe, table[pe].physNode);
    else
      CmiPrintf("PE %d: physical node %d, %d CPU(s) in mask, first CPU %d\n",
                pe, table[pe].physNode, table[pe].cpuCount, table[pe].firstCpu);
  }

  if (CpvAccess(expectBound)) {
    for (int pe = 0; pe < CmiNumPes(); ++pe) {
      if (table[pe].cpuCount != 1)
        CmiAbort("PE %d is not bound to a single CPU (%d in its mask) although "
                 "an affinity map was given\n",
                 pe, table[pe].cpuCount);
      for (int other = 0; other < pe; ++other) {
        if (table[other].physNode == table[pe].physNode &&
            table[other].firstCpu == table[pe].firstCpu)
          CmiAbort("PEs %d and %d on physical node %d are both bound to CPU "
                   "%d: the +pemap entries were not applied per PE\n",
                   other, pe, table[pe].physNode, table[pe].firstCpu);
      }
    }
    CmiPrintf("Affinity: every PE bound to its own CPU\n");
  } else {
    CmiPrintf("Affinity: flags accepted; binding not checked on this "
              "platform\n");
  }

  startIpcPhase();
}

// ============================================================ phase 2: IPC ==

static void startDirectPhase();

#ifdef CMK_USE_SHMEM

// Runs in a suspendable thread on every PE: the bootstrap Charm++'s init.C
// performs, then the sends.
static void ipcThreadFn(void *) {
  CthThread self = CthSelf();
  CmiIpcManager *manager = CmiMakeIpcManager(self);
  if (CmiMyRank() == 0)
    CsvAccess(coreIpcManager_) = manager;
  // CmiMakeIpcManager wakes this thread once every process on the host has
  // attached every other's segment.
  CthSuspend();

  int other = 1 - CmiMyNode();
  if (CmiPhysicalNodeID(CmiMyPe()) != CmiPhysicalNodeID(CmiNodeFirst(other)))
    CmiAbort("PE %d: the two processes are on different physical nodes; the "
             "IPC phase needs both on one host\n",
             CmiMyPe());

  // A block for one's own process must be refused, not handed out: the
  // scheduler only polls the queue for blocks other processes push.
  std::pair<CmiIpcBlock *, CmiIpcAllocStatus> own =
      CmiAllocIpcBlock(manager, CmiMyNode(), 256);
  if (own.first != nullptr || own.second != CMI_IPC_REMOTE_DESTINATION)
    CmiAbort("PE %d: CmiAllocIpcBlock for my own process returned status %d "
             "instead of CMI_IPC_REMOTE_DESTINATION\n",
             CmiMyPe(), (int)own.second);

  // The other process initializes its own segment only after the pid
  // exchange reaches it, so for a moment after this thread wakes an allocation
  // there reports CMI_IPC_TIMEOUT. Charm++ rides this out by falling back to a
  // network send; this test has to wait instead, bounded, and must never see
  // any other failure.
  {
    const int maxAttempts = 50000; // 5 s at 100 us
    int attempt = 0;
    for (;; ++attempt) {
      std::pair<CmiIpcBlock *, CmiIpcAllocStatus> probe =
          CmiAllocIpcBlock(manager, other, 64);
      if (probe.second == CMI_IPC_SUCCESS) {
        CmiFreeIpcBlock(manager, probe.first);
        break;
      }
      if (probe.second != CMI_IPC_TIMEOUT)
        CmiAbort("PE %d: CmiAllocIpcBlock for process %d failed with status "
                 "%d (%s)\n",
                 CmiMyPe(), other, (int)probe.second,
                 probe.second == CMI_IPC_OUT_OF_MEMORY ? "out of memory"
                                                        : "remote destination");
      if (attempt == maxAttempts)
        CmiAbort("PE %d: process %d's shared segment did not become ready "
                 "within %d attempts\n",
                 CmiMyPe(), other, maxAttempts);
      usleep(100);
    }
  }

  for (int i = 0; i < numIpcSizes; ++i) {
    size_t len = ipcSizes[i];
    size_t total = sizeof(IpcMsg) + len;
    IpcMsg *msg = newMsg<IpcMsg>(CpvAccess(ipcRecvHIdx), len);
    msg->fromPe = CmiMyPe();
    msg->toPe = ipcPartnerPe();
    msg->payloadLen = len;
    fillPattern((char *)(msg + 1), len, (unsigned int)len);

    // ck.C's _tryIpcSend: copy into a block of the destination's segment
    // addressed to the destination rank, then push; a full queue is retried.
    // Timeout 0: retry a transient CMI_IPC_TIMEOUT (another allocator holds
    // the heap pointer) indefinitely; any other failure returns null.
    CmiIpcBlock *block = CmiMsgToIpcBlock(manager, (char *)msg, total, other,
                                          CmiRankOf(msg->toPe), 0);
    if (block == nullptr)
      CmiAbort("PE %d: could not get an IPC block of %zu bytes for process "
               "%d\n",
               CmiMyPe(), total, other);
    while (!CmiPushIpcBlock(manager, block))
      ;
  }
}

static void ipcStartHandler(void *vmsg) {
  CmiFree(vmsg);
  CthThread th = CthCreate(ipcThreadFn, nullptr, 0);
  CthAwaken(th);
}

// Delivered by the scheduler's CmiPopIpcBlock poll; msg lives in this
// process's shared segment.
static void ipcRecvHandler(void *vmsg) {
  IpcMsg *msg = (IpcMsg *)vmsg;
  if (msg->toPe != CmiMyPe())
    CmiAbort("PE %d: IPC block addressed to PE %d was delivered here\n",
             CmiMyPe(), msg->toPe);
  if (msg->fromPe != ipcPartnerPe())
    CmiAbort("PE %d: IPC block from PE %d, expected PE %d\n", CmiMyPe(),
             msg->fromPe, ipcPartnerPe());
  checkPattern("IPC block payload", (const char *)(msg + 1), msg->payloadLen,
               (unsigned int)msg->payloadLen);

  CmiIpcManager *manager = CsvAccess(coreIpcManager_);
  if (CmiMsgToIpcBlock(manager, vmsg) == nullptr)
    CmiAbort("PE %d: a message delivered from the IPC queue is not recognised "
             "as an IPC block of this process\n",
             CmiMyPe());
  // CmiFree recognises the block and returns it to the segment's free list.
  CmiFree(vmsg);

  if (++CpvAccess(ipcReceived) == numIpcSizes)
    sendPlain(0, CpvAccess(ipcDoneHIdx));
}

static void ipcDoneHandler(void *vmsg) {
  CmiFree(vmsg);
  if (++CpvAccess(ipcDoneReports) < CmiNumPes())
    return;
  CmiPrintf("IPC: %d blocks of %d sizes delivered between the two processes "
            "on every PE\n",
            numIpcSizes * CmiNumPes(), numIpcSizes);
  startDirectPhase();
}

static void startIpcPhase() {
  CmiPrintf("IPC: shared-memory blocks (CMK_USE_SHMEM)\n");
  CmiSyncBroadcastAllAndFree(sizeof(PlainMsg),
                             newMsg<PlainMsg>(CpvAccess(ipcStartHIdx)));
}

#else // !CMK_USE_SHMEM

static void ipcStartHandler(void *vmsg) { CmiFree(vmsg); }
static void ipcRecvHandler(void *vmsg) { CmiFree(vmsg); }
static void ipcDoneHandler(void *vmsg) { CmiFree(vmsg); }

static void startIpcPhase() {
  CmiPrintf("IPC: skipped, this build has CMK_USE_SHMEM off\n");
  startDirectPhase();
}

#endif

// ===================================== phase 3: Direct API with dereg ==

static void allocBuf(size_t len, unsigned int seed) {
  CmiAssert(CpvAccess(bufBase) == nullptr);
  char *base = (char *)CmiAlloc(len);
  fillPattern(base, len, seed);
  CpvAccess(bufBase) = base;
  CpvAccess(bufLen) = len;
  CpvAccess(localBuf) = CmiNcpyBuffer(base, len);
  CpvAccess(peerDeregistered) = 0;
}

static void freeBuf() {
  // On the peer the layer already deregistered this buffer while serving the
  // initiator's dereg request; deregistering it twice is not part of the
  // contract, so only deregister what is still registered.
  if (CpvAccess(peerDeregistered))
    CpvAccess(localBuf).isRegistered = false;
  CpvAccess(localBuf).deregisterMem();
  CmiFree(CpvAccess(bufBase));
  CpvAccess(bufBase) = nullptr;
  CpvAccess(bufLen) = 0;
}

static void startSize();

// Acknowledgement handler for every Direct API operation in this process,
// modelled on Charm++'s CkRdmaDirectAckHandler.
static void directAckHandler(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;

  if (info->opMode == CMK_DIRECT_API) {
    // The one acknowledgement the layer owes the initiator.
    if (CmiMyNode() != CmiNodeOf(initiatorPe()))
      CmiAbort("PE %d: CMK_DIRECT_API acknowledgement in a process that "
               "initiated nothing\n",
               CmiMyPe());
    if (info->ackMode != CMK_SRC_DEST_ACK)
      CmiAbort("PE %d: initiator acknowledgement has ackMode %d, expected "
               "CMK_SRC_DEST_ACK (%d)\n",
               CmiMyPe(), (int)info->ackMode, (int)CMK_SRC_DEST_ACK);
    int acks = directAcks.fetch_add(1) + 1;
    if (acks != 1)
      CmiAbort("PE %d: %d acknowledgements for one Direct API operation\n",
               CmiMyPe(), acks);
    // Who owns info is what its freeMe says: on the RMA path it is the
    // CmiAlloc'd object the initiator created (CMK_FREE_NCPYOPINFO); on the
    // copy-based path it lives inside the message that carried the bytes
    // (CMK_DONT_FREE_NCPYOPINFO). Charm++ reads the field; so does the dereg
    // call below, which is why freeing by opMode was wrong (reconverse #221).
    if (info->freeMe != CMK_FREE_NCPYOPINFO &&
        info->freeMe != CMK_DONT_FREE_NCPYOPINFO)
      CmiAbort("PE %d: initiator's NcpyOperationInfo has freeMe %d\n",
               CmiMyPe(), (int)info->freeMe);

    // Charm++: deregister the other side's buffer and run its callback there.
    // The layer frees info if freeMe says so, and the message holding it is
    // freed once this handler returns otherwise, so nothing may touch info
    // after this call. Charm++ pairs this with QdCreate(1).
    deregRequests.fetch_add(1);
    CmiInvokeRemoteDeregAckHandler(peerPe(), info);

    // The initiator PE checks the data and advances; this may not be it.
    sendPlain(initiatorPe(), CpvAccess(initiatorDoneHIdx));
    return;
  }

  if (info->opMode == CMK_EM_API_SRC_ACK_INVOKE ||
      info->opMode == CMK_EM_API_DEST_ACK_INVOKE) {
    // The other side's callback, delivered by the dereg round trip. Charm++
    // pairs it with QdProcess(1).
    if (CmiMyPe() != peerPe())
      CmiAbort("PE %d: dereg acknowledgement (opMode %d) on the wrong PE\n",
               CmiMyPe(), (int)info->opMode);
    if (++CpvAccess(remoteAcks) != 1)
      CmiAbort("PE %d: %d dereg acknowledgements for one operation\n",
               CmiMyPe(), CpvAccess(remoteAcks));
    CpvAccess(remoteAcksTotal) += 1;
    if (info->freeMe != CMK_DONT_FREE_NCPYOPINFO)
      CmiAbort("PE %d: dereg acknowledgement's info has freeMe %d; it lives "
               "inside the dereg message and must not be freed by the "
               "caller\n",
               CmiMyPe(), (int)info->freeMe);

    if (CpvAccess(phase) == PHASE_GET) {
      // This PE was the source of the get.
      if (info->opMode != CMK_EM_API_SRC_ACK_INVOKE ||
          info->ackMode != CMK_SRC_ACK || info->isSrcRegistered != 0 ||
          CmiNodeOf(info->srcPe) != CmiMyNode())
        CmiAbort("PE %d: dereg acknowledgement for the get's source has "
                 "opMode %d, ackMode %d, isSrcRegistered %d\n",
                 CmiMyPe(), (int)info->opMode, (int)info->ackMode,
                 (int)info->isSrcRegistered);
    } else {
      // This PE was the destination of the put; the bytes must be here.
      if (info->opMode != CMK_EM_API_DEST_ACK_INVOKE ||
          info->ackMode != CMK_DEST_ACK || info->isDestRegistered != 0 ||
          CmiNodeOf(info->destPe) != CmiMyNode())
        CmiAbort("PE %d: dereg acknowledgement for the put's destination has "
                 "opMode %d, ackMode %d, isDestRegistered %d\n",
                 CmiMyPe(), (int)info->opMode, (int)info->ackMode,
                 (int)info->isDestRegistered);
      checkPattern("data received by rdmaPut", CpvAccess(bufBase),
                   CpvAccess(bufLen), SOURCE_SEED);
    }
    CpvAccess(peerDeregistered) = 1;
    freeBuf();
    CpvAccess(remoteAcks) = 0;

    CountMsg *done = newMsg<CountMsg>(CpvAccess(peerDoneHIdx));
    done->count = CpvAccess(remoteAcksTotal);
    CmiSyncSendAndFree(initiatorPe(), sizeof(CountMsg), done);
    return;
  }

  CmiAbort("PE %d: acknowledgement with unexpected opMode %d\n", CmiMyPe(),
           (int)info->opMode);
}

// Initiator: ask the peer for a registered buffer for the current size and
// phase. After a get the peer's buffer has been deregistered by the round
// trip, so the put needs a fresh one; reusing the descriptor would put into
// unregistered memory.
static void requestPeerBuffer(int phase) {
  CmiAssert(CmiMyPe() == initiatorPe());
  CpvAccess(phase) = phase;
  SizeMsg *msg = newMsg<SizeMsg>(CpvAccess(prepareHIdx));
  msg->len = directSizes[CpvAccess(sizeIdx)];
  msg->phase = phase;
  CmiSyncSendAndFree(peerPe(), sizeof(SizeMsg), msg);
}

// Initiator: next size, or finish.
static void startSize() {
  CmiAssert(CmiMyPe() == initiatorPe());
  if (CpvAccess(sizeIdx) == numDirectSizes) {
    CountMsg *done = newMsg<CountMsg>(CpvAccess(directFinishedHIdx));
    done->count = deregRequests.load();
    CmiSyncSendAndFree(0, sizeof(CountMsg), done);
    return;
  }
  requestPeerBuffer(PHASE_GET);
}

static void directStartHandler(void *vmsg) {
  CmiFree(vmsg);
  CmiPrintf("Direct API: PE %d <-> PE %d, %s, with the deregistration round "
            "trip\n",
            initiatorPe(), peerPe(),
            CmiUseCopyBasedRDMA ? "copy-based path" : "network RDMA path");
  startSize();
}

// Peer: register a buffer of this size and hand its descriptor over. Before a
// get it holds the pattern the initiator must receive; before a put it holds
// a pattern the put must overwrite.
static void prepareHandler(void *vmsg) {
  SizeMsg *msg = (SizeMsg *)vmsg;
  size_t len = msg->len;
  int phase = msg->phase;
  CmiFree(msg);

  allocBuf(len, phase == PHASE_GET ? TARGET_SEED : POISON_SEED);
  CpvAccess(phase) = phase;
  CpvAccess(remoteAcks) = 0;

  BufMsg *reply = newMsg<BufMsg>(CpvAccess(bufReadyHIdx));
  reply->buf = CpvAccess(localBuf);
  CmiSyncSendAndFree(initiatorPe(), sizeof(BufMsg), reply);
}

// Initiator: the peer's buffer is ready; pull from it or push into it.
static void bufReadyHandler(void *vmsg) {
  BufMsg *msg = (BufMsg *)vmsg;
  CpvAccess(remoteBuf) = msg->buf;
  CmiFree(msg);

  directAcks.store(0);
  CpvAccess(signals) = 0;
  if (CpvAccess(phase) == PHASE_GET) {
    allocBuf(CpvAccess(remoteBuf).cnt, POISON_SEED);
    CpvAccess(localBuf).rdmaGet(CpvAccess(remoteBuf), 0, nullptr, nullptr);
  } else {
    allocBuf(CpvAccess(remoteBuf).cnt, SOURCE_SEED);
    CpvAccess(localBuf).rdmaPut(CpvAccess(remoteBuf), 0, nullptr, nullptr);
  }
}

// Initiator: a phase is over once its own acknowledgement has been seen and
// the peer has reported its dereg acknowledgement.
static void phaseSignal() {
  CmiAssert(CmiMyPe() == initiatorPe());
  if (++CpvAccess(signals) != 2)
    return;

  freeBuf();
  if (CpvAccess(phase) == PHASE_GET) {
    requestPeerBuffer(PHASE_PUT);
    return;
  }

  CmiPrintf("Size=%zu bytes: rdmaGet and rdmaPut acknowledged once each, "
            "dereg round trips answered\n",
            directSizes[CpvAccess(sizeIdx)]);
  CpvAccess(phase) = PHASE_IDLE;
  CpvAccess(sizeIdx) += 1;
  startSize();
}

// Initiator: its own acknowledgement was raised (possibly on another PE of
// this process). After a get the bytes are local, so check them here.
static void initiatorDoneHandler(void *vmsg) {
  CmiFree(vmsg);
  if (CpvAccess(phase) == PHASE_GET)
    checkPattern("data received by rdmaGet", CpvAccess(bufBase),
                 CpvAccess(bufLen), TARGET_SEED);
  phaseSignal();
}

// Initiator: the peer answered a dereg request. Its running count of
// answered requests must equal the number this PE has issued, or Charm++'s
// QdCreate/QdProcess pairing would be off by the difference.
static void peerDoneHandler(void *vmsg) {
  CountMsg *msg = (CountMsg *)vmsg;
  int answered = msg->count;
  CmiFree(msg);
  if (answered != deregRequests.load())
    CmiAbort("PE %d: %d dereg requests issued but the peer has answered %d\n",
             CmiMyPe(), deregRequests.load(), answered);
  phaseSignal();
}

// PE 0: all phases done.
static void directFinishedHandler(void *vmsg) {
  CountMsg *msg = (CountMsg *)vmsg;
  CmiPrintf("Direct API: %d sizes, %d dereg round trips, each answered "
            "exactly once\n",
            numDirectSizes, msg->count);
  CmiFree(msg);
  CmiPrintf("runtime_modes: all phases passed\n");
  CmiSyncBroadcastAllAndFree(sizeof(PlainMsg),
                             newMsg<PlainMsg>(CpvAccess(exitHIdx)));
}

static void startDirectPhase() {
  sendPlain(initiatorPe(), CpvAccess(directStartHIdx));
}

static void exitHandler(void *vmsg) {
  CmiFree(vmsg);
  if (CpvAccess(bufBase) != nullptr)
    freeBuf();
  CsdExitScheduler();
}

// ------------------------------------------------------------------ main ---

#define REGISTER(var, fn)                                                      \
  do {                                                                         \
    CpvInitialize(int, var);                                                   \
    CpvAccess(var) = CmiRegisterHandler((CmiHandler)fn);                       \
  } while (0)

static void runtimeModesInit(int argc, char **argv) {
  CpvInitialize(int, expectBound);
  CpvAccess(expectBound) = CmiGetArgFlagDesc(
      argv, "-expect-bound", "require every PE to be bound to one CPU");

  REGISTER(affinityHIdx, affinityHandler);
  REGISTER(ipcStartHIdx, ipcStartHandler);
  REGISTER(ipcRecvHIdx, ipcRecvHandler);
  REGISTER(ipcDoneHIdx, ipcDoneHandler);
  REGISTER(directStartHIdx, directStartHandler);
  REGISTER(prepareHIdx, prepareHandler);
  REGISTER(bufReadyHIdx, bufReadyHandler);
  REGISTER(initiatorDoneHIdx, initiatorDoneHandler);
  REGISTER(peerDoneHIdx, peerDoneHandler);
  REGISTER(directFinishedHIdx, directFinishedHandler);
  REGISTER(exitHIdx, exitHandler);

  CpvInitialize(int, affinityReports);
  CpvInitialize(AffinityMsg *, affinityTable);
  CpvAccess(affinityTable) = nullptr;
  CpvInitialize(int, ipcReceived);
  CpvInitialize(int, ipcDoneReports);
  CpvInitialize(int, phase);
  CpvAccess(phase) = PHASE_IDLE;
  CpvInitialize(int, sizeIdx);
  CpvInitialize(int, signals);
  CpvInitialize(int, remoteAcks);
  CpvInitialize(int, remoteAcksTotal);
  CpvInitialize(int, peerDeregistered);
  CpvInitialize(char *, bufBase);
  CpvAccess(bufBase) = nullptr;
  CpvInitialize(size_t, bufLen);
  CpvInitialize(CmiNcpyBuffer, localBuf);
  CpvInitialize(CmiNcpyBuffer, remoteBuf);

  CmiSetDirectNcpyAckHandler(directAckHandler);

  if (CmiNumNodes() != 2 || CmiNumPes() % 2 != 0 || CmiNumPes() < 2)
    CmiAbort("This test needs exactly 2 processes with the same number of PEs "
             "each, on one host. Run it as: <launcher> -n 2 "
             "./reconverse_runtime_modes +pe 4 +nordma +pemap L0-3\n");

  // What Charm++'s init.C does on every PE, in this order. Reconverse's
  // ConverseInit does neither.
  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  CmiCheckAffinity();
#ifdef CMK_USE_SHMEM
  CmiIpcInit(argv);
#endif

  if (CmiMyPe() == 0) {
    CpvAccess(affinityTable) =
        (AffinityMsg *)malloc(CmiNumPes() * sizeof(AffinityMsg));
    CmiPrintf("runtime_modes: %d PEs in 2 processes; affinity, %s, Direct API "
              "(%s)\n",
              CmiNumPes(),
#ifdef CMK_USE_SHMEM
              "shared-memory IPC",
#else
              "no IPC (CMK_USE_SHMEM off)",
#endif
              CmiUseCopyBasedRDMA ? "copy-based" : "network RDMA");
  }
  reportAffinity();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, runtimeModesInit);
}
