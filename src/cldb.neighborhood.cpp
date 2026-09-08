// Neighborhood-averaging seed load balancer over a C-regular random graph.
//
// Phase 1 (no priorities). Design:
//  - Virtual topology: every PE is a vertex of a C-regular random graph
//    (near-Ramanujan spectral gap => diffusion mixes in O(log N) rounds).
//    Deterministic generation from a shared seed: each PE computes the same
//    graph and keeps only its own row; no startup broadcast.
//  - Seeds created with CK_PE_ANY (CLD_ANYWHERE) go into a per-PE DEQUE:
//    local execution pops the TOP (newest => depth-first traversal of a
//    divide-and-conquer tree, memory stays proportional to depth); exports
//    peel the BOTTOM (oldest => shallowest => chunkiest subtasks).
//  - Sender-initiated balancing (neighborhood averaging): when this PE's
//    load exceeds the average over {self} U neighbors, ship each
//    under-average neighbor its deficit, as ONE combined batch message.
//  - Load information travels piggybacked on every batch, plus explicit
//    status messages suppressed unless the load changed by statusDelta
//    since the last value sent to that neighbor.
//
// The deque is owner-only: seeds arriving from neighbors come in as normal
// converse messages whose handler runs on this PE's thread. No locks.
//
// Scheduler integration: two poll handlers contributed to the table-driven
// scheduler via CldAddPollHandlers() (called from CmiQueueRegisterInitThread):
//    pollSeedDeque   - execute one seed from the top (also re-checks balance
//                      after each grain so loaded PEs react within one grain)
//    pollSeedBalance - status + balance check (matters when the deque is
//                      empty or the PE is idle)
//
// Runtime knobs (command line):
//    +cldC <even int>       graph degree (default 6)
//    +cldStatusDelta <int>  min load change before a status resend (default 2)
//    +cldBatchMax <int>     max seeds per batch message (default 64)
//    +cldSeedStats          print per-PE seed-balancer stats at exit

#include "cldb.h"
#include "cldb_regular_graph.h"
#include "converse_internal.h"
#include "scheduler.h"
#include <cstring>
#include <deque>
#include <vector>

void LoadNotifyFn(int) {}

const char *CldGetStrategy() { return "neighborhood"; }

// ---------------------------------------------------------------------------
// Per-PE state (one scheduler thread per PE)
// ---------------------------------------------------------------------------

static thread_local std::deque<char *> seedDeque; // top = back, bottom = front
static thread_local std::vector<int> nbrs;        // neighbor PE ids
static thread_local std::vector<int> nbrLoad;     // last known load per nbr
static thread_local std::vector<int> lastSent;    // last load sent per nbr

static thread_local int CldSeedBatchHandlerIndex;
static thread_local int CldStatusHandlerIndex;

// knobs (same values on every PE; parsed from the command line)
static thread_local int cldC = 6;
static thread_local int statusDelta = 2;
static thread_local int batchMax = 64;
static thread_local bool seedStats = false;

// stats
static thread_local long statSeedsLocal = 0;    // seeds enqueued by this PE
static thread_local long statSeedsExecuted = 0; // seeds run on this PE
static thread_local long statSeedsImported = 0;
static thread_local long statSeedsExported = 0;
static thread_local long statBatches = 0;
static thread_local long statStatusMsgs = 0;

static constexpr uint64_t GRAPH_SEED = 0x5EEDBA1AULL;

#define CLD_ALIGN8(x) (((x) + 7) & ~7)

// ---------------------------------------------------------------------------
// Message formats
// ---------------------------------------------------------------------------

struct StatusMsg {
  char core[CmiMsgHeaderSizeBytes];
  int srcPe;
  int load;
};

// Batch layout (zero-copy delivery): after the BatchHeader, each seed record
// is [CmiChunkHeader][seed bytes], aligned to ALIGN_BYTES. The embedded
// chunk header's ref field holds a NEGATIVE byte offset back to the batch
// allocation's own header — CmiFree/CmiAllocFindEnclosing interpret that as
// "sub-message of an enclosing block" and decrement the batch's refcount
// instead. The receiver therefore pushes INTERIOR POINTERS straight into its
// deque (no per-seed alloc or copy); the batch block is freed automatically
// when its last seed is executed (charm frees it) or re-exported.
// Offsets are relative, so they survive the byte-copy across the wire.
struct BatchHeader {
  char core[CmiMsgHeaderSizeBytes];
  int srcPe;
  int srcLoad; // sender's load AFTER this export (piggybacked status)
  int count;
};

#define CLD_ALIGN_CHUNK(x) (((x) + (ALIGN_BYTES - 1)) & ~(ALIGN_BYTES - 1))

// ---------------------------------------------------------------------------
// Load bookkeeping helpers
// ---------------------------------------------------------------------------

static inline int myLoad() { return static_cast<int>(seedDeque.size()); }

static int nbrIndexOf(int pe) {
  for (size_t i = 0; i < nbrs.size(); ++i)
    if (nbrs[i] == pe)
      return static_cast<int>(i);
  return -1;
}

static void sendStatusIfStale(int i, int load) {
  if (std::abs(load - lastSent[i]) < statusDelta)
    return;
  StatusMsg *m = (StatusMsg *)CmiAlloc(sizeof(StatusMsg));
  m->srcPe = CmiMyPe();
  m->load = load;
  CmiSetHandler(m, CldStatusHandlerIndex);
  CmiSyncSendAndFree(nbrs[i], sizeof(StatusMsg), (char *)m);
  lastSent[i] = load;
  ++statStatusMsgs;
}

// ---------------------------------------------------------------------------
// Export path: peel seeds off the BOTTOM, pack into one message per neighbor
// ---------------------------------------------------------------------------

// Diagnostic path (+cldNoBatch): ship each seed as its own message through
// the classic CldSwitchHandler route — no copy, no batching. Dest executes
// via its Csd queue rather than its deque.
static thread_local bool noBatch = false;

static void exportSingles(int nbrIdx, int want) {
  int destPe = nbrs[nbrIdx];
  int sent = 0;
  while (sent < want && !seedDeque.empty()) {
    char *msg = seedDeque.front();
    seedDeque.pop_front();
    int len, queueing, priobits;
    unsigned int *prioptr;
    CldPackFn pfn;
    CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNodeOf(destPe) != CmiMyNode()) {
      pfn((void **)&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler(msg, CpvAccess(CldHandlerIndex));
    CmiSyncSendAndFree(destPe, len, msg);
    ++sent;
  }
  statSeedsExported += sent;
  CldRelocatedMessages += sent;
  nbrLoad[nbrIdx] += sent;
  lastSent[nbrIdx] = myLoad();
}

static void exportBatch(int nbrIdx, int want) {
  if (noBatch) {
    exportSingles(nbrIdx, want);
    return;
  }
  int destPe = nbrs[nbrIdx];
  bool crossNode = (CmiNodeOf(destPe) != CmiMyNode());
  int count = 0;
  int totalBytes = CLD_ALIGN_CHUNK(sizeof(BatchHeader));

  // First pass: gather up to `want` seeds (bottom-first) and their lengths.
  std::vector<char *> picked;
  std::vector<int> lens;
  picked.reserve(want);
  while (count < want && !seedDeque.empty()) {
    char *msg = seedDeque.front();
    seedDeque.pop_front();
    int len, queueing, priobits;
    unsigned int *prioptr;
    CldPackFn pfn;
    CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    // Pack UNCONDITIONALLY before serializing into the batch: an unpacked
    // envelope is not a self-contained byte string (it may carry live
    // pointers), and unlike CmiSyncSendAndFree-of-the-original, a memcpy'd
    // copy plus CmiFree of the original leaves those dangling. The charm
    // handler unpacks on delivery (isPacked check), same as network arrival.
    if (pfn) {
      pfn((void **)&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    (void)crossNode;
    picked.push_back(msg);
    lens.push_back(len);
    totalBytes += sizeof(CmiChunkHeader) + CLD_ALIGN_CHUNK(len);
    ++count;
  }
  if (count == 0)
    return;

  BatchHeader *batch = (BatchHeader *)CmiAlloc(totalBytes);
  batch->srcPe = CmiMyPe();
  batch->srcLoad = myLoad();
  batch->count = count;
  // Each record: an embedded CmiChunkHeader whose ref = -(byte offset from
  // this embedded header back to the batch allocation's header), followed by
  // the seed bytes. `batch` points just past the batch's own CmiChunkHeader.
  char *p = (char *)batch + CLD_ALIGN_CHUNK(sizeof(BatchHeader));
  for (int k = 0; k < count; ++k) {
    CmiChunkHeader *h = (CmiChunkHeader *)p;
    h->size = lens[k];
    h->mr = nullptr;
    h->setRef(-(int)((char *)h -
                     ((char *)batch - sizeof(CmiChunkHeader))));
    std::memcpy(p + sizeof(CmiChunkHeader), picked[k], lens[k]);
    CmiFree(picked[k]);
    p += sizeof(CmiChunkHeader) + CLD_ALIGN_CHUNK(lens[k]);
  }
  CmiSetHandler(batch, CldSeedBatchHandlerIndex);
  CmiSyncSendAndFree(destPe, totalBytes, (char *)batch);

  statSeedsExported += count;
  ++statBatches;
  CldRelocatedMessages += count;
  // Optimistic bookkeeping so the next balance round doesn't double-send
  // before the neighbor's own status arrives.
  nbrLoad[nbrIdx] += count;
  lastSent[nbrIdx] = myLoad(); // srcLoad piggybacked above
}

// Neighborhood averaging (DMCC'90 Fig. 3, adapted): if above the average of
// {self} U neighbors, raise each under-average neighbor toward the average,
// bottom-first, one combined message per neighbor.
static void maybeBalance() {
  int n = static_cast<int>(nbrs.size());
  if (n == 0)
    return;

  long sum = myLoad();
  for (int i = 0; i < n; ++i)
    sum += nbrLoad[i];
  int avg = static_cast<int>(sum / (n + 1));

  int surplus = myLoad() - avg;
  if (surplus <= 0) {
    // Not overloaded: just keep neighbors' picture of us fresh.
    for (int i = 0; i < n; ++i)
      sendStatusIfStale(i, myLoad());
    return;
  }

  for (int i = 0; i < n && surplus > 0; ++i) {
    if (nbrLoad[i] >= avg)
      continue;
    int want = avg - nbrLoad[i];
    if (want > surplus)
      want = surplus;
    if (want > batchMax)
      want = batchMax;
    exportBatch(i, want);
    surplus = myLoad() - avg;
  }
}

// ---------------------------------------------------------------------------
// Poll handlers (registered in the table-driven scheduler)
// ---------------------------------------------------------------------------

static bool pollSeedDeque() {
  if (seedDeque.empty())
    return false;
  char *msg = seedDeque.back(); // TOP: newest => depth-first
  seedDeque.pop_back();
  if (CmiGetIdle()) {
    CmiSetIdle(false);
    CcdRaiseCondition(CcdPROCESSOR_END_IDLE);
  }
  ++statSeedsExecuted;
  CmiHandleMessage(msg);
  // One balance check per executed grain: loaded PEs react to a neighbor
  // going idle within a single grain, not a full 64-slot sweep.
  maybeBalance();
  return true;
}

static bool pollSeedBalance() {
  maybeBalance();
  return false; // bookkeeping, not "work" — idle accounting stays truthful
}

void CldAddPollHandlers(
    std::vector<std::pair<QueuePollHandlerFn, unsigned int>> &handlers) {
  handlers.push_back(std::make_pair(pollSeedDeque, 8));
  handlers.push_back(std::make_pair(pollSeedBalance, 1));
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

static void CldStatusHandler(void *vmsg) {
  StatusMsg *m = static_cast<StatusMsg *>(vmsg);
  int i = nbrIndexOf(m->srcPe);
  if (i >= 0)
    nbrLoad[i] = m->load;
  CmiFree(m);
}

// Delivery policy, decided at the RECEIVER per batch:
//  - same-node batches: zero-copy (the block handed over is the sender's own
//    allocation; nothing is pinned).
//  - cross-node batches: copy each seed out and free the batch immediately —
//    zero-copy there pins the transport's receive buffer until the last seed
//    is consumed, which starves the pool (measured: ~15% slowdown, laptop).
// Overrides for experiments: +cldCopyAtDest forces copying everywhere;
// +cldZCRemote forces zero-copy everywhere.
static thread_local bool copyAtDest = false;
static thread_local bool zcRemote = false;

static void CldSeedBatchHandler(void *vmsg) {
  BatchHeader *batch = static_cast<BatchHeader *>(vmsg);
  int i = nbrIndexOf(batch->srcPe);
  if (i >= 0)
    nbrLoad[i] = batch->srcLoad;
  char *p = (char *)batch + CLD_ALIGN_CHUNK(sizeof(BatchHeader));
  bool doCopy =
      copyAtDest ||
      (!zcRemote && CmiNodeOf(batch->srcPe) != CmiMyNode());
  if (doCopy) {
    for (int k = 0; k < batch->count; ++k) {
      CmiChunkHeader *h = (CmiChunkHeader *)p;
      char *seed = (char *)CmiAlloc(h->size);
      std::memcpy(seed, p + sizeof(CmiChunkHeader), h->size);
      seedDeque.push_back(seed);
      p += sizeof(CmiChunkHeader) + CLD_ALIGN_CHUNK(h->size);
    }
    statSeedsImported += batch->count;
    CmiFree(batch);
    return;
  }
  // Zero-copy: hand the interior seed pointers to the deque. Ownership of
  // the batch block transfers to the seeds — each seed's eventual CmiFree
  // walks its embedded header's negative ref to the batch header and
  // decrements it; the block dies with its last seed. We hold `count`
  // owners total: the handler's own reference is inherited by the first
  // seed, and count-1 more are added here. This handler must NOT free.
  for (int k = 1; k < batch->count; ++k)
    CmiReference(batch);
  for (int k = 0; k < batch->count; ++k) {
    CmiChunkHeader *h = (CmiChunkHeader *)p;
    char *seed = p + sizeof(CmiChunkHeader);
    seedDeque.push_back(seed); // imported chunky seeds go on top: run next
    p += sizeof(CmiChunkHeader) + CLD_ALIGN_CHUNK(h->size);
  }
  statSeedsImported += batch->count;
}

// Handlers for explicit-destination messages (same roles as in cldb.rand).
static void CldHandler(void *msg) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldRestoreHandler(static_cast<char *>(msg));
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
}

static void CldNodeHandler(void *msg) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldRestoreHandler(static_cast<char *>(msg));
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CmiGetNodeQueue()->push(msg);
}

// ---------------------------------------------------------------------------
// CldEnqueue family
// ---------------------------------------------------------------------------

void CldEnqueue(int pe, void *msg, int infofn) {
  if (pe == CLD_ANYWHERE) {
    // The whole point: a new seed stays HERE until balancing moves it.
    CmiSetInfo(msg, infofn);
    seedDeque.push_back(static_cast<char *>(msg)); // top
    ++statSeedsLocal;
    return;
  }
  // Explicit destination: same as the rand strategy.
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  if (pe == CmiMyPe() && !CmiImmIsRunning()) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNodeOf(pe) != CmiMyNode()) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
    CmiSetInfo(msg, infofn);
    if (pe == CLD_BROADCAST) {
      CmiSyncBroadcastAndFree(len, msg);
    } else if (pe == CLD_BROADCAST_ALL) {
      CmiSyncBroadcastAllAndFree(len, msg);
    } else {
      CmiSyncSendAndFree(pe, len, msg);
    }
  }
}

void CldNodeEnqueue(int node, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  if (node == CLD_ANYWHERE) {
    // Treat node-anywhere as PE-anywhere: keep it in the local deque.
    CldEnqueue(CLD_ANYWHERE, msg, infofn);
    return;
  }
  if (node == CmiMyNode() && !CmiImmIsRunning()) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CldNodeHandlerIndex);
    CmiSetInfo(msg, infofn);
    if (node == CLD_BROADCAST) {
      CmiSyncNodeBroadcastAndFree(len, msg);
    } else if (node == CLD_BROADCAST_ALL) {
      CmiSyncNodeBroadcastAllAndFree(len, msg);
    } else
      CmiSyncNodeSendAndFree(node, len, msg);
  }
}

// Group/multicast/within-node variants: same as cldb.rand.
void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg, infofn);
  CmiSyncMulticastAndFree(grp, len, msg);
}

void CldEnqueueWithinNode(void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldPackFn pfn;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn && !CMI_MSG_NOKEEP(msg)) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg, infofn);
  CmiWithinNodeBroadcast(len, (char *)msg);
}

void CldEnqueueMulti(int npes, const int *pes, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg, infofn);
  CmiSyncListSendAndFree(npes, pes, len, msg);
}

// ---------------------------------------------------------------------------
// Init / exit
// ---------------------------------------------------------------------------

void CldSeedStatsPrint() {
  CmiPrintf("[cld] PE %d: local %ld exec %ld imported %ld exported %ld "
            "batches %ld status %ld finalLoad %d\n",
            CmiMyPe(), statSeedsLocal, statSeedsExecuted, statSeedsImported,
            statSeedsExported, statBatches, statStatusMsgs, myLoad());
}

void CldModuleInit(char **argv) {
  CpvInitialize(int, CldHandlerIndex);
  CpvAccess(CldHandlerIndex) = CmiRegisterHandler((CmiHandler)CldHandler);
  CldNodeHandlerIndex = CmiRegisterHandler((CmiHandler)CldNodeHandler);
  CldSeedBatchHandlerIndex =
      CmiRegisterHandler((CmiHandler)CldSeedBatchHandler);
  CldStatusHandlerIndex = CmiRegisterHandler((CmiHandler)CldStatusHandler);
  CldRelocatedMessages = 0;
  CldLoadBalanceMessages = 0;
  CldMessageChunks = 0;

  CmiGetArgInt(argv, "+cldC", &cldC);
  CmiGetArgInt(argv, "+cldStatusDelta", &statusDelta);
  CmiGetArgInt(argv, "+cldBatchMax", &batchMax);
  seedStats = CmiGetArgFlag(argv, "+cldSeedStats");
  noBatch = CmiGetArgFlag(argv, "+cldNoBatch");
  copyAtDest = CmiGetArgFlag(argv, "+cldCopyAtDest");
  zcRemote = CmiGetArgFlag(argv, "+cldZCRemote");
  if (cldC % 2)
    ++cldC; // undirected regular graph needs even degree

  int N = CmiNumPes();
  int me = CmiMyPe();
  if (N - 1 <= cldC) {
    // Too few PEs for a simple C-regular graph: fall back to the clique.
    for (int p = 0; p < N; ++p)
      if (p != me)
        nbrs.push_back(p);
  } else {
    // Deterministic: every PE builds the identical graph, keeps its row.
    RegularGraph g(N, cldC, GRAPH_SEED);
    nbrs = g.neighbors(me);
  }
  nbrLoad.assign(nbrs.size(), 0);
  lastSent.assign(nbrs.size(), 0);

  if (me == 0)
    CmiPrintf("Charm++> neighborhood-averaging seed LB: C=%d (|nbrs|=%d), "
              "statusDelta=%d, batchMax=%d over %d PEs\n",
              cldC, (int)nbrs.size(), statusDelta, batchMax, N);

  CldModuleGeneralInit(argv);
}

void CldCallback() {
  if (seedStats)
    CldSeedStatsPrint();
}
