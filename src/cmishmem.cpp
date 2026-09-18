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

inline static CmiIpcBlock* popBlock_(std::atomic<std::uintptr_t>& head,
                                     void* base);
inline static bool pushBlock_(std::atomic<std::uintptr_t>& head,
                              std::uintptr_t value, void* base);
static std::uintptr_t allocBlock_(ipc_shared_* meta, std::size_t size);

void* CmiIpcBlockToMsg(CmiIpcBlock* block, bool init) {
  auto* msg = (char*)CmiIpcBlockToMsg(block);
  if (init) {
    // NOTE ( this is identical to code in CmiAlloc )
    CmiAssert(((uintptr_t)msg % ALIGN_BYTES) == 0);
    CMI_ZC_MSGTYPE((void*) msg) = CMK_REG_NO_ZC_MSG;
    CmiSetMsgNokeep((void *)msg, 0);
    SIZEFIELD(msg) = block->size;
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

CmiIpcBlock* CmiPopIpcBlock(CmiIpcManager* meta) {
  if (metadataReady_(meta)) {
    auto* shared = meta->shared[meta->mine].load(std::memory_order_acquire);
    return popBlock_(shared->queue, shared);
  } else {
    return nullptr;
  }
}

bool CmiPushIpcBlock(CmiIpcManager* meta, CmiIpcBlock* block) {
  auto* shared = meta->shared[block->src].load(std::memory_order_acquire);
  auto& queue = shared->queue;
  CmiAssert(meta->mine == block->dst);
  return pushBlock_(queue, block->orig, shared);
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
  if (shared == nullptr) {
    // the destination's segment hasn't been attached yet (startup window)
    return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_TIMEOUT);
  }
  auto bin = whichBin_(size);
  CmiAssert(bin < kNumCutOffPoints);

  auto* block = popBlock_(shared->free[bin], shared);
  if (block == nullptr) {
    auto totalSize = kCutOffPoints[bin];
    auto offset = allocBlock_(shared, totalSize);
    switch (offset) {
      case cmi::ipc::nil:
        return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_TIMEOUT);
      case cmi::ipc::max:
        return std::make_pair((CmiIpcBlock*)nullptr, CMI_IPC_OUT_OF_MEMORY);
      default:
        break;
    }
    // the block's address is relative to the share
    block = (CmiIpcBlock*)((char*)shared + offset);
    CmiAssert(((std::uintptr_t)block % alignof(CmiIpcBlock)) == 0);
    // construct the block
    new (block) CmiIpcBlock(totalSize, offset);
  }

  block->src = dstProc;
  block->dst = thisProc;

  return std::make_pair(block, CMI_IPC_SUCCESS);
}

void CmiFreeIpcBlock(CmiIpcManager* meta, CmiIpcBlock* block) {
  auto bin = whichBin_(block->size);
  CmiAssert(bin < kNumCutOffPoints);
  auto* shared = meta->shared[block->src].load(std::memory_order_acquire);
  auto& free = shared->free[bin];
  while (!pushBlock_(free, block->orig, shared))
    ;
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

static std::uintptr_t allocBlock_(ipc_shared_* meta, std::size_t size) {
  auto res = meta->heap.exchange(cmi::ipc::nil, std::memory_order_acquire);
  if (res == cmi::ipc::nil) {
    return cmi::ipc::nil;
  } else {
    auto next = res + size + sizeof(CmiIpcBlock);
    auto offset = size % alignof(CmiIpcBlock);
    auto oom = next >= meta->max;
    auto value = oom ? res : (next + offset);
    auto status = meta->heap.exchange(value, std::memory_order_release);
    CmiAssert(status == cmi::ipc::nil);
    if (oom) {
      return cmi::ipc::max;
    } else {
      return res;
    }
  }
}

inline static CmiIpcBlock* popBlock_(std::atomic<std::uintptr_t>& head,
                                     void* base) {
  auto prev = head.exchange(cmi::ipc::nil, std::memory_order_acquire);
  if (prev == cmi::ipc::nil) {
    return nullptr;
  } else if (prev == cmi::ipc::max) {
    auto check = head.exchange(prev, std::memory_order_release);
    CmiAssert(check == cmi::ipc::nil);
    return nullptr;
  } else {
    // translate the "home" PE's address into a local one
    CmiAssert(((std::uintptr_t)base % ALIGN_BYTES) == 0);
    auto* xlatd = (CmiIpcBlock*)((char*)base + prev);
    auto check = head.exchange(xlatd->next, std::memory_order_release);
    CmiAssert(check == cmi::ipc::nil);
    return xlatd;
  }
}

inline static bool pushBlock_(std::atomic<std::uintptr_t>& head,
                              std::uintptr_t value, void* base) {
  CmiAssert(value != cmi::ipc::nil);
  auto prev = head.exchange(cmi::ipc::nil, std::memory_order_acquire);
  if (prev == cmi::ipc::nil) {
    return false;
  }
  auto* block = (CmiIpcBlock*)((char*)base + value);
  block->next = prev;
  auto check = head.exchange(value, std::memory_order_release);
  CmiAssert(check == cmi::ipc::nil);
  return true;
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

bool CmiIpcTrySendAndFree(int destNode, int destRank, int messageSize,
                          void* msg) {
  auto* manager = CsvAccess(coreIpcManager_);
  if (manager == nullptr) return false;
  // Acquire-load of the flag the setup code released: it orders this PE's
  // reads of peers[] (written once, by PE 0, during setup) after those
  // writes, and it keeps sends off a pool whose segments are not all mapped.
  if (!manager->ready.load(std::memory_order_acquire)) return false;
  // one load rules out every destination the pool cannot reach
  if (destNode < 0 || destNode >= (int)manager->peers.size() ||
      !manager->peers[destNode])
    return false;
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
