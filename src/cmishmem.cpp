// Shared-memory IPC pool: message passing between processes that share a
// host, bypassing the network backend entirely.
//
// This file holds everything that does not depend on how a peer's pool is
// mapped -- the block allocator, the send/receive queues, argument parsing,
// startup and the counters -- and includes the backends that do. Keeping
// them in one translation unit is deliberate: the sleeper list, the manager
// registry and the Cpv/Csv state below are shared by every backend, and
// CsvStaticDeclare is a plain file-static.
#include "cmi-shmem-common.h"
#include "converse_config.h"

#include <algorithm>
#include <cstring>

#define CMI_DEST_RANK(msg) ((CmiMsgHeaderBasic*)msg)->destPE

// ---------------------------------------------------------------------------
// state shared by the backends
// ---------------------------------------------------------------------------

// which backend this run settled on, and whether it is usable yet
CsvStaticDeclare(int, ipcMode_);
// requested on the command line but not yet bootstrapped
CsvStaticDeclare(int, ipcRequested_);
CpvStaticDeclare(int, ipcInitDone_);
CpvStaticDeclare(long, ipcSent_);
CpvStaticDeclare(long, ipcRecvd_);
// Nesting depth of CmiIpcBeginNetworkOnly(): while it is nonzero this PE's
// sends stay on the backend, so they cannot overtake a network operation the
// caller already has in flight.
CpvStaticDeclare(int, ipcNetworkOnly_);

static void awakenSleepers_(void) {
  auto& current_sleepers = CsvAccess(sleepers);
  for (std::size_t i = 0; i < current_sleepers.size(); i++) {
    auto& th = current_sleepers[i];
    // A PE that asked for the pool without a thread to park (reconverse's own
    // startup, and Charm++'s communication thread) leaves a null here.
    if (th == nullptr) continue;
    if ((int)i == CmiMyRank()) {
      CthAwaken(th);
    } else {
      auto* token = CthGetToken(th);
      CmiSetHandler(token, CpvAccess(CthResumeNormalThreadIdx));
      CmiPushPE(i, token);
    }
    th = nullptr;
  }
}

// Called by a backend once every peer segment on this host is mapped. Records
// which logical nodes those peers are -- the send path tests that one array
// instead of walking the topology -- and publishes the pool.
static void finishSetup_(CmiIpcManager* meta) {
  int* pes;
  int nPes;
  CmiGetPesOnPhysicalNode(CmiPhysicalNodeID(CmiMyPe()), &pes, &nPes);
  const int nSize = CmiMyNodeSize();
  const int nProcs = nPes / nSize;
  meta->nPeers = nProcs;
  for (int i = 0; i < nProcs; i++) {
    const int proc = CmiNodeOf(pes[i * nSize]);
    if (proc != meta->mine && proc >= 0 && proc < (int)meta->peers.size())
      meta->peers[proc] = 1;
  }
  // Each PE's index among the host's PEs names its return lists in every
  // peer's segment, and each rank has one receive queue in its own.
  CmiEnforceMsg(nSize <= kMaxIpcRanks && nPes <= kMaxIpcHostPes,
                "shared-memory pool: %d PEs per process and %d per host; at "
                "most %d and %d are supported (kMaxIpcRanks, kMaxIpcHostPes)",
                nSize, nPes, kMaxIpcRanks, kMaxIpcHostPes);
  for (int r = 0; r < nSize; r++) {
    const int pe = CmiNodeFirst(meta->mine) + r;
    auto& st = meta->pe[r];
    st.hostIdx = (int)(std::find(pes, pes + nPes, pe) - pes);
    CmiEnforceMsg(st.hostIdx < nPes, "shared-memory pool: PE %d is not on its "
                  "own host's PE list", pe);
    st.cache.resize(CmiNumNodes());
    for (auto& c : st.cache) c.fill(cmi::ipc::max);
    st.inbox.reserve(256);
  }
  meta->ready.store(true, std::memory_order_release);
}

#include "cmishm.cpp"
#if CMK_HAS_XPMEM
#include "cmixpmem.cpp"
#endif

// ---------------------------------------------------------------------------
// block pool
// ---------------------------------------------------------------------------

extern void CmiPushNode(void* msg);

// Pushes a block onto one of a segment's lists (see ipc_shared_). Any number
// of PEs, in any process, may push at once.
inline static void pushList_(std::atomic<std::uintptr_t>& head,
                             CmiIpcBlock* block) {
  auto prev = head.load(std::memory_order_relaxed);
  do {
    block->next = prev;
  } while (!head.compare_exchange_weak(prev, block->orig,
                                       std::memory_order_release,
                                       std::memory_order_relaxed));
}

// Takes a whole list (newest first) and appends it to out, so that popping
// out from the back yields the oldest block first. Only the list's one taker
// calls this; it writes the head only when the list is not empty.
inline static void takeList_(std::atomic<std::uintptr_t>& head,
                             ipc_shared_* shared,
                             std::vector<CmiIpcBlock*>& out) {
  if (head.load(std::memory_order_relaxed) == cmi::ipc::max) return;
  auto off = head.exchange(cmi::ipc::max, std::memory_order_acquire);
  while (off != cmi::ipc::max) {
    auto* block = (CmiIpcBlock*)((char*)shared + off);
    out.push_back(block);
    off = block->next;
  }
}

void* CmiIpcBlockToMsg(CmiIpcBlock* block, bool init) {
  auto* msg = (char*)CmiIpcBlockToMsg(block);
  if (init) {
    // NOTE ( this is identical to code in CmiAlloc )
    CmiAssert(((uintptr_t)msg % ALIGN_BYTES) == 0);
    CMI_ZC_MSGTYPE((void*) msg) = CMK_REG_NO_ZC_MSG;
    CmiSetMsgNokeep((void *)msg, 0);
    // Pool memory is not registered with the backend, and nothing else ever
    // writes this field. A handler that forwards a message it received from
    // the pool to a PE on another host passes MRFIELD straight to issueAm,
    // and LCI2 dereferences any non-null value, so say there is no region
    // here rather than relying on the segment's pages starting out zeroed.
    MRFIELD(msg) = comm_backend::MR_NULL;
    // block->size covers the chunk header too, so the space a message can
    // use is that much smaller -- CmiSize must not promise the difference.
    // A sender overwrites this with the message's real length below, which
    // is what CmiAlloc stores.
    CmiAssert(block->size > sizeof(CmiChunkHeader));
    SIZEFIELD(msg) = block->size - sizeof(CmiChunkHeader);
    REFFIELDSET(msg, 1);
  }
  return msg;
}

CmiIpcBlock* CmiMsgToIpcBlock(CmiIpcManager* manager, char* src, std::size_t len,
                           int node, int rank, int timeout) {
  char* dst;
  CmiIpcBlock* block;
  // check whether we miraculously got a usable block
  if ((block = CmiIsIpcBlock(manager, BLKSTART(src), node)) && (node == block->src)) {
    dst = src;
  } else {
    std::pair<CmiIpcBlock*, CmiIpcAllocStatus> status;
    // we only want to attempt again if we fail due to a timeout:
    if (timeout > 0) {
      do {
        status = CmiAllocIpcBlock(manager, node, len + sizeof(CmiChunkHeader));
      } while ((--timeout) && (status.second == CMI_IPC_TIMEOUT));
    } else {
      do {
        status = CmiAllocIpcBlock(manager, node, len + sizeof(CmiChunkHeader));
        // never give up, never surrender!
      } while (status.second == CMI_IPC_TIMEOUT);
    }
    // grab the block from the rval
    block = status.first;
    if (block == nullptr) {
      return nullptr;
    } else {
      CmiAssert((block->dst == manager->mine) && (manager->mine == CmiMyNode()));
      dst = (char*)CmiIpcBlockToMsg(block, true);
      memcpy(dst, src, len);
      CmiFree(src);
    }
  }
  CMI_DEST_RANK(dst) = rank;
  // The block came out of a bin, so it is generally larger than the message
  // in it. Record what was actually written, the way CmiAlloc records the
  // requested size: a receiver that writes up to CmiSize would otherwise run
  // off the end of the message and into the next block's header. Freeing is
  // unaffected -- CmiFreeIpcBlock bins by block->size.
  SIZEFIELD(dst) = len;
  return block;
}

extern void CmiHandleImmediateMessage(void *msg);

void CmiDeliverIpcBlockMsg(CmiIpcBlock* block) {
  auto* msg = CmiIpcBlockToMsg(block);
  auto& dest = CMI_DEST_RANK(msg);
  if (CpvInitialized(ipcRecvd_)) CpvAccess(ipcRecvd_)++;
  // The sender put a *rank* (or the node-datagram marker) in the header's
  // destination field, because that is all the pool needs to route within
  // this process. Put back what a message off the network would carry, so a
  // handler cannot tell the two apart: reconverse's own receive path and
  // Charm++ both read this field as a global PE number.
  if (dest == cmi::ipc::nodeDatagram) {
    dest = CmiMessageDestPENode;
    CmiPushNode(msg);
  } else {
    const int rank = (int)dest;
    dest = (CmiUInt4)(CmiNodeFirst(CmiMyNode()) + rank);
    CmiPushPE(rank, msg);
  }
}

inline static bool metadataReady_(CmiIpcManager* meta) {
  return meta && meta->shared[meta->mine].load(std::memory_order_acquire);
}

// Refills this PE's inbox with everything its receive queue and the process's
// node queue hold; returns whether it got any.
static bool refillInbox_(CmiIpcManager* meta, ipc_pe_state_& st) {
  auto* shared = meta->shared[meta->mine].load(std::memory_order_acquire);
  takeList_(shared->rq[CmiMyRank()].head, shared, st.inbox);
  takeList_(shared->node_rq.head, shared, st.inbox);
  return !st.inbox.empty();
}

CmiIpcBlock* CmiPopIpcBlock(CmiIpcManager* meta) {
  if (!metadataReady_(meta) || !meta->ready.load(std::memory_order_acquire))
    return nullptr;
  auto& st = meta->pe[CmiMyRank()];
  if (st.inbox.empty() && !refillInbox_(meta, st)) return nullptr;
  auto* block = st.inbox.back();
  st.inbox.pop_back();
  return block;
}

int CmiDeliverIpcInbox(CmiIpcManager* meta) {
  if (!metadataReady_(meta) || !meta->ready.load(std::memory_order_acquire))
    return 0;
  auto& st = meta->pe[CmiMyRank()];
  if (st.inbox.empty() && !refillInbox_(meta, st)) return 0;
  // Only what this pass took: delivering moves a message onto a local queue,
  // and the scheduler runs it on a later sweep.
  int n = 0;
  while (!st.inbox.empty()) {
    auto* block = st.inbox.back();
    st.inbox.pop_back();
    CmiDeliverIpcBlockMsg(block);
    n++;
  }
  return n;
}

bool CmiPushIpcBlock(CmiIpcManager* meta, CmiIpcBlock* block) {
  auto* shared = meta->shared[block->src].load(std::memory_order_acquire);
  CmiAssert(meta->mine == block->dst);
  // CmiMsgToIpcBlock put the destination rank (or the node marker) here
  const auto rank = CMI_DEST_RANK(CmiIpcBlockToMsg(block));
  if (rank == cmi::ipc::nodeDatagram) {
    pushList_(shared->node_rq.head, block);
  } else {
    CmiAssert(rank < (CmiUInt4)kMaxIpcRanks);
    pushList_(shared->rq[rank].head, block);
  }
  return true;
}

std::pair<CmiIpcBlock*, CmiIpcAllocStatus> CmiAllocIpcBlock(CmiIpcManager* meta, int dstProc, std::size_t size) {
  auto dstNode = CmiPhysicalNodeID(CmiNodeFirst(dstProc));
  auto thisPe = CmiMyPe();
  auto thisProc = CmiMyNode();
  auto thisNode = CmiPhysicalNodeID(thisPe);
  if ((thisProc == dstProc) || (thisNode != dstNode)) {
    return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_REMOTE_DESTINATION);
  }

  auto* shared = meta->shared[dstProc].load(std::memory_order_acquire);
  if (shared == nullptr || !meta->ready.load(std::memory_order_acquire)) {
    // the destination's segment hasn't been attached yet (startup window)
    return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_TIMEOUT);
  }
  auto bin = whichBin_(size);
  if (bin >= (std::size_t)kIpcRetBins) {
    // larger than any block the return lists cover; the caller uses the network
    return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_OUT_OF_MEMORY);
  }

  // This PE's private list of blocks of that size in that segment, refilled
  // with everything its receivers there have returned since it last looked
  auto& st = meta->pe[CmiMyRank()];
  auto& head = st.cache[dstProc][bin];
  if (head == cmi::ipc::max) {
    head = shared->ret[st.hostIdx].bin[bin].exchange(
        cmi::ipc::max, std::memory_order_acquire);
  }
  CmiIpcBlock* block;
  if (head != cmi::ipc::max) {
    block = (CmiIpcBlock*)((char*)shared + head);
    head = block->next;
  } else {
    // a new block from the segment's heap
    if (shared->heap.load(std::memory_order_acquire) == cmi::ipc::nil) {
      // the owner has not initialized its segment yet
      return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_TIMEOUT);
    }
    auto totalSize = kCutOffPoints[bin];
    auto stride = sizeof(CmiIpcBlock) + totalSize;
    stride += (alignof(CmiIpcBlock) - stride % alignof(CmiIpcBlock)) %
              alignof(CmiIpcBlock);
    auto offset = shared->heap.fetch_add(stride, std::memory_order_relaxed);
    if (offset + stride > shared->max) {
      return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_OUT_OF_MEMORY);
    }
    // the block's address is relative to the share
    block = (CmiIpcBlock*)((char*)shared + offset);
    CmiAssert(((std::uintptr_t)block % alignof(CmiIpcBlock)) == 0);
    new (block) CmiIpcBlock(totalSize, offset);
  }

  block->owner = st.hostIdx;
  block->src = dstProc;
  block->dst = thisProc;

  return std::make_pair(block, CMI_IPC_SUCCESS);
}

void CmiFreeIpcBlock(CmiIpcManager* meta, CmiIpcBlock* block) {
  auto bin = whichBin_(block->size);
  CmiAssert(bin < (std::size_t)kIpcRetBins);
  CmiAssert(block->owner >= 0 && block->owner < kMaxIpcHostPes);
  auto* shared = meta->shared[block->src].load(std::memory_order_acquire);
  // back to the PE that allocated it, which takes its list whole
  pushList_(shared->ret[block->owner].bin[bin], block);
}

CmiIpcBlock* CmiIsIpcBlock(CmiIpcManager* meta, void* addr, int node) {
  auto* shared =
      meta ? meta->shared[node].load(std::memory_order_acquire) : nullptr;
  if (shared == nullptr) {
    return nullptr;
  }
  auto* begin = (char*)shared;
  auto* end = begin + shared->max;
  if (begin < addr && addr < end) {
    return (CmiIpcBlock*)((char*)addr - sizeof(CmiIpcBlock));
  } else {
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

static const char* ipcModeName_(int mode) {
  switch (mode) {
    case CMI_IPC_MODE_POSIX_SHM: return "posixshm";
    case CMI_IPC_MODE_XPMEM: return "xpmem";
    default: return "none";
  }
}

// Whether xpmem is compiled in and this host's kernel actually offers it.
static bool ipcXpmemAvailable_(void) {
#if CMK_HAS_XPMEM
  return ipcXpmemUsable_();
#else
  return false;
#endif
}

// Picks the backend for this run. Every process on a host sees the same
// answer, because the only run-time input is whether that host's kernel
// exposes xpmem.
static int chooseIpcMode_(const char* requested) {
  if (requested == nullptr || strcmp(requested, "auto") == 0) {
    return ipcXpmemAvailable_() ? CMI_IPC_MODE_XPMEM : CMI_IPC_MODE_POSIX_SHM;
  }
  if (strcmp(requested, "off") == 0 || strcmp(requested, "none") == 0) {
    return CMI_IPC_MODE_OFF;
  }
  if (strcmp(requested, "shm") == 0 || strcmp(requested, "posixshm") == 0 ||
      strcmp(requested, "pxshm") == 0) {
    return CMI_IPC_MODE_POSIX_SHM;
  }
  if (strcmp(requested, "xpmem") == 0) {
#if !CMK_HAS_XPMEM
    CmiAbort("+ipcmode xpmem: this reconverse was built without xpmem support "
             "(configure with -DRECONVERSE_ENABLE_XPMEM=ON and an xpmem "
             "installation on the search path)");
#endif
    if (!ipcXpmemAvailable_())
      CmiAbort("+ipcmode xpmem: /dev/xpmem is not available here -- the xpmem "
               "kernel module does not appear to be loaded on this host");
    return CMI_IPC_MODE_XPMEM;
  }
  CmiAbort("+ipcmode: unknown mode '%s' (expected auto, shm, xpmem or off)",
           requested);
  return CMI_IPC_MODE_OFF;
}

void CmiIpcInit(char** argv) {
  // Charm++ calls this too; whoever gets here first sets the run up.
  if (CpvInitialized(ipcInitDone_) && CpvAccess(ipcInitDone_)) return;
  CpvInitialize(int, ipcInitDone_);
  CpvAccess(ipcInitDone_) = 1;
  CpvInitialize(long, ipcSent_);
  CpvInitialize(long, ipcRecvd_);
  CpvInitialize(int, ipcNetworkOnly_);

  CsvInitialize(ipc_manager_map_, managers_);

  initSleepers_();
  initSegmentSize_(argv);

  // Register every backend's handlers whichever one is in use: handler ids
  // are assigned in call order and have to match across processes, and two
  // hosts in one job can disagree about whether xpmem is available.
  ipcInitShm_(argv);
#if CMK_HAS_XPMEM
  ipcInitXpmem_(argv);
#endif

  // The backend a pool would use if one is built. Charm++ drives its own
  // setup and never touches the flags below, so this is what it gets.
  if (CmiMyRank() == 0) CsvAccess(ipcMode_) = chooseIpcMode_(nullptr);
}

// Parses the flags that decide whether reconverse itself uses the pool, and
// consumes them whether or not it does. Runs on every PE.
void CmiIpcCliInit(char** argv) {
  char* modeArg = nullptr;
  const int on = CmiGetArgFlagDesc(
      argv, "+ipc", "use shared memory between processes on the same host");
  const int off = CmiGetArgFlagDesc(argv, "+noipc",
                                    "do not use shared memory between "
                                    "processes on the same host (default)");
  CmiGetArgStringDesc(argv, "+ipcmode", &modeArg,
                      "shared-memory mechanism to use: auto, shm or xpmem");

  CmiIpcInit(argv);

  int mode = CMI_IPC_MODE_OFF;
  if (!off && (on || modeArg != nullptr)) {
    mode = chooseIpcMode_(modeArg);
  }
  if (CmiMyRank() == 0) {
    CsvAccess(ipcRequested_) = (mode != CMI_IPC_MODE_OFF);
    if (mode != CMI_IPC_MODE_OFF) CsvAccess(ipcMode_) = mode;
  }
  CmiNodeAllBarrier();
}

int CmiIpcRequested(void) { return CsvAccess(ipcRequested_); }

// Brings the pool up and blocks until every process on this host has mapped
// every other's segment. Called by every PE from converseRunPe, before the
// user's start function, so that a program can send over the pool from its
// very first message.
void CmiIpcStartup(void) {
  if (!CsvAccess(ipcRequested_)) return;

  if (CmiNumNodes() == 1) {
    if (CmiMyPe() == 0)
      CmiPrintf("Converse> +ipc ignored: this job has a single process, so "
                "there is no peer to share memory with.\n");
    if (CmiMyRank() == 0) CsvAccess(ipcRequested_) = 0;
    CmiNodeAllBarrier();
    return;
  }

  CmiIpcManager* manager = CmiMakeIpcManager(nullptr);
  if (CmiMyRank() == 0) CsvAccess(coreIpcManager_) = manager;
  CmiNodeAllBarrier();

  // Drive the scheduler until the exchange finishes. Only bootstrap messages
  // are in flight here: no process leaves this function before the barrier
  // below, and nothing has run the user's code yet.
  const double deadline = CmiWallTimer() + 120.0;
  while (!manager->ready.load(std::memory_order_acquire)) {
    CsdSchedulePoll();
    if (CmiWallTimer() > deadline)
      CmiAbort("+ipc: timed out setting up the %s pool on PE %d",
               ipcModeName_(CsvAccess(ipcMode_)), CmiMyPe());
  }
  CmiNodeAllBarrier();

  // Nobody may start sending until every peer has mapped our segment, and
  // nobody may leave startup until every peer's pool is usable.
  CmiBarrier();
}

CLINKAGE int CmiIpcEnabled(void) {
  auto* manager = CsvAccess(coreIpcManager_);
  return manager != nullptr && manager->ready.load(std::memory_order_acquire);
}

CLINKAGE const char* CmiIpcImplName(void) {
  return CmiIpcEnabled() ? ipcModeName_(CsvAccess(ipcMode_)) : "none";
}

CLINKAGE int CmiIpcNumPeers(void) {
  auto* manager = CsvAccess(coreIpcManager_);
  return manager ? manager->nPeers : 1;
}

CLINKAGE long CmiIpcMessagesSent(void) {
  return CpvInitialized(ipcSent_) ? CpvAccess(ipcSent_) : 0;
}

CLINKAGE long CmiIpcMessagesReceived(void) {
  return CpvInitialized(ipcRecvd_) ? CpvAccess(ipcRecvd_) : 0;
}

CLINKAGE void CmiIpcBeginNetworkOnly(void) {
  // Uninitialized means CmiIpcInit has not run, so there is no pool to keep
  // sends off of and nothing to do.
  if (CpvInitialized(ipcNetworkOnly_)) CpvAccess(ipcNetworkOnly_)++;
}

CLINKAGE void CmiIpcEndNetworkOnly(void) {
  if (!CpvInitialized(ipcNetworkOnly_)) return;
  CmiEnforceMsg(CpvAccess(ipcNetworkOnly_) > 0,
                "CmiIpcEndNetworkOnly without a matching "
                "CmiIpcBeginNetworkOnly");
  CpvAccess(ipcNetworkOnly_)--;
}

bool CmiIpcReaches(int destNode) {
  auto* manager = CsvAccess(coreIpcManager_);
  if (manager == nullptr) return false;
  // Acquire-load of the flag the setup code released: it orders this PE's
  // reads of peers[] (written once, by PE 0, during setup) after those
  // writes, and it keeps sends off a pool whose segments are not all mapped.
  if (!manager->ready.load(std::memory_order_acquire)) return false;
  // one load rules out every destination the pool cannot reach
  return destNode >= 0 && destNode < (int)manager->peers.size() &&
         manager->peers[destNode];
}

bool CmiIpcTrySendAndFree(int destNode, int destRank, int messageSize,
                          void* msg) {
  if (!CmiIpcReaches(destNode)) return false;
  // The caller is inside a CmiIpcBeginNetworkOnly() scope: it has a network
  // operation in flight that this message must not overtake, so the pool is
  // not an option here however well it would fit.
  if (CpvAccess(ipcNetworkOnly_) > 0) return false;
  auto* manager = CsvAccess(coreIpcManager_);
  if ((std::size_t)messageSize > CmiRecommendedIpcBlockCutoff()) return false;

  auto* block = CmiMsgToIpcBlock(manager, (char*)msg, (std::size_t)messageSize,
                                 destNode, destRank, cmi::ipc::defaultTimeout);
  if (block == nullptr) return false;
  // the receiver's queue is only ever blocked by another pusher, briefly
  while (!CmiPushIpcBlock(manager, block))
    ;
  CpvAccess(ipcSent_)++;
  return true;
}

CmiIpcManager* CmiMakeIpcManager(CthThread th) {
  putSleeper_(th);

  // ensure all sleepers are reg'd
  CmiNodeAllBarrier();

  // reconverse's own startup may already have brought a pool up; hand that
  // one back rather than building a second pool nobody would drain
  if (auto* existing = CsvAccess(coreIpcManager_)) {
    if (CmiMyRank() == 0) awakenSleepers_();
    CmiNodeAllBarrier();
    return existing;
  }

  CmiIpcManager* manager = nullptr;
  if (CmiMyRank() == 0) {
    auto key = CsvAccess(managers_).size() + 1;
    switch (CsvAccess(ipcMode_)) {
#if CMK_HAS_XPMEM
      case CMI_IPC_MODE_XPMEM:
        manager = ipcMakeManagerXpmem_(key);
        break;
#endif
      default:
        manager = ipcMakeManagerShm_(key);
        break;
    }
    // register before bootstrapping: a peer's reply is looked up by key
    CsvAccess(managers_).emplace_back(manager);
    switch (CsvAccess(ipcMode_)) {
#if CMK_HAS_XPMEM
      case CMI_IPC_MODE_XPMEM:
        ipcBootstrapXpmem_(manager);
        break;
#endif
      default:
        ipcBootstrapShm_(manager);
        break;
    }
    // signal the metadata is ready
    CmiNodeAllBarrier();
    return manager;
  } else {
    // pause until the metadata is ready
    CmiNodeAllBarrier();
    return CsvAccess(managers_).back().get();
  }
}
