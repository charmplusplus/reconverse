// Regions, counters and targets, and the data paths within a process.
// See doc/internals/persistent-family.md; design references are to
// charm's doc/persistent-family-design.md.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

#include "conv-rdma.h"
#include "conv-region-internal.h"

// convcore.cpp: frees a message once the backend has sent it
void CommLocalHandler(comm_backend::Status status);

using namespace cmi_region;

namespace {

// The sentence design 11 ends every readiness abort with.
const char kReadinessAdvice[] =
    "If the application's logic cannot guarantee this transfer's readiness, "
    "use a message send here.";

CounterSlice *g_slices = nullptr;
uint32_t g_linesPerChunk = 0;
uint32_t g_slotsPerChunk = 0;
uint32_t g_counterLimit = kMaxCountersPerPe;
std::atomic<CmiRegionQdFn> g_qdHook{nullptr};

// How data and signals reach another process; set at start-up, the same in
// every process.
bool g_useRma = false;       // one-sided gets, and regions registered
bool g_usePutSignal = false; // puts with signal
int g_putKinds[2] = {-1, -1}; // the backend's kinds, by SignalKind
size_t g_putMessageMax = 0;   // a put of at most this many bytes is a message
int g_signalAm = -1;          // the active message of CmiSignal
bool g_verbose = false;       // +region_verbose
std::atomic<bool> g_pathShown[2] = {{false}, {false}};
#if CMI_REGION_CHECKS
int g_completionDelayUs = 0; // +region_test_completion_delay
#endif

void *alignedAlloc(size_t align, size_t bytes, const char *what) {
  void *p = nullptr;
#ifdef _WIN32
  p = _aligned_malloc(bytes, align);
#else
  if (posix_memalign(&p, align, bytes) != 0) p = nullptr;
#endif
  if (p == nullptr)
    CmiAbort("%s: could not allocate %zu bytes\n", what, bytes);
  return p;
}

void alignedFree(void *p) {
#ifdef _WIN32
  _aligned_free(p);
#else
  free(p);
#endif
}

void checkOwner(int ownerPe, const char *call, const char *what) {
  if (ownerPe != CmiMyPe())
    CmiAbort("%s: the %s belongs to PE %d; call this on that PE, not on PE %d\n",
             call, what, ownerPe, CmiMyPe());
}

} // namespace

CpvStaticDeclare(PeState *, regionPeState);
CpvStaticDeclare(int, regionFireHandlerIdx);
CpvStaticDeclare(int, regionDoneHandlerIdx);
CpvStaticDeclare(int, regionPutDataHandlerIdx);
CpvStaticDeclare(int, regionGetRequestHandlerIdx);
CpvStaticDeclare(int, regionGetReplyHandlerIdx);

namespace cmi_region {

PeState *peState() { return CpvAccess(regionPeState); }

CounterLine *lineOf(int rank, uint32_t index) {
  if (rank < 0 || rank >= CmiMyNodeSize() || index >= kMaxCountersPerPe)
    return nullptr;
  CounterLine *chunk = g_slices[rank]
                           .chunks[index / g_linesPerChunk]
                           .load(std::memory_order_acquire);
  return chunk ? chunk + index % g_linesPerChunk : nullptr;
}

RegionSlot *regionSlotOf(int rank, uint32_t slot) {
  if (rank < 0 || rank >= CmiMyNodeSize() || slot >= kMaxRegionsPerPe)
    return nullptr;
  RegionSlot *chunk = g_slices[rank]
                          .slotChunks[slot / g_slotsPerChunk]
                          .load(std::memory_order_acquire);
  return chunk ? chunk + slot % g_slotsPerChunk : nullptr;
}

void qdCount(int created, int processed) {
  CmiRegionQdFn fn = g_qdHook.load(std::memory_order_acquire);
  if (fn != nullptr) fn(created, processed);
}

#if CMI_REGION_CHECKS
void testCompletionDelay() {
  if (g_completionDelayUs <= 0) return;
  double end = CmiWallTimer() + g_completionDelayUs * 1e-6;
  while (CmiWallTimer() < end) {
  }
}
#endif

void postDone(CmiDoneFn fn, void *arg) {
  if (fn == nullptr) return;
  auto *m = static_cast<DoneMsg *>(CmiAlloc(sizeof(DoneMsg)));
  m->fn = fn;
  m->arg = arg;
  CmiSetHandler(m, CpvAccess(regionDoneHandlerIdx));
  reinterpret_cast<CmiMessageHeader *>(m)->messageSize = sizeof(DoneMsg);
  CmiPushPE(CmiMyRank(), m);
}

} // namespace cmi_region

namespace {

// The line of a new counter: chunks are allocated on first use by the owner
// PE, the only writer of its chunk table, and published with a release store
// for the threads that look a line up.
CounterLine *allocLine(PeState *ps, uint32_t index) {
  std::atomic<CounterLine *> &slot =
      g_slices[ps->rank].chunks[index / g_linesPerChunk];
  CounterLine *chunk = slot.load(std::memory_order_relaxed);
  if (chunk == nullptr) {
    size_t page = std::max<size_t>(CmiGetPageSize(), CMI_CACHE_LINE_SIZE);
    chunk = static_cast<CounterLine *>(alignedAlloc(
        page, g_linesPerChunk * sizeof(CounterLine), "CmiCounterCreate"));
    for (uint32_t i = 0; i < g_linesPerChunk; i++) new (&chunk[i]) CounterLine();
    slot.store(chunk, std::memory_order_release);
  }
  return chunk + index % g_linesPerChunk;
}

// A region slot, allocated as allocLine allocates lines.
RegionSlot *allocSlot(PeState *ps, uint32_t slot) {
  std::atomic<RegionSlot *> &entry =
      g_slices[ps->rank].slotChunks[slot / g_slotsPerChunk];
  RegionSlot *chunk = entry.load(std::memory_order_relaxed);
  if (chunk == nullptr) {
    chunk = new RegionSlot[g_slotsPerChunk];
    for (uint32_t i = 0; i < g_slotsPerChunk; i++) {
      chunk[i].landed.store(0, std::memory_order_relaxed);
      chunk[i].generation.store(0, std::memory_order_relaxed);
      chunk[i].pad = 0;
    }
    entry.store(chunk, std::memory_order_release);
  }
  return chunk + slot % g_slotsPerChunk;
}

#if CMI_REGION_CHECKS
int ownerPeOfLine(const CounterLine *L) {
  return CmiNodeFirst(CmiMyNode()) + L->ownerRank;
}

[[noreturn]] void abortReadiness(const char *call, const CounterLine *L,
                                 const char *fmt, ...) {
  char what[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(what, sizeof(what), fmt, ap);
  va_end(ap);
  CmiAbort("%s: counter %u (generation %u) of PE %d, round %u: %s. %s\n", call,
           L->index, L->generation.load(std::memory_order_relaxed),
           ownerPeOfLine(L), L->armedRounds.load(std::memory_order_relaxed),
           what, kReadinessAdvice);
}
#endif

// A contribution of weight k (design 4.2, 5.3): the one that takes the word
// to zero queues the counter's message on the owner PE. The message is read
// before the decrement, so a destroy that completes right after the
// decrement cannot hand this thread another counter's message. The count
// check is for a target's counter; a local counter's completion that comes
// before its re-arm is no readiness error, and counts toward the next round.
inline void contribute(CounterLine *L, int32_t k, const char *call,
                       bool checked = true) {
  void *fire = L->fireMsg.load(std::memory_order_relaxed);
  int32_t old = L->word.fetch_sub(k, std::memory_order_acq_rel);
#if CMI_REGION_CHECKS
  if (checked && old < k)
    abortReadiness(call, L,
                   "a contribution arrived after the round fired and before "
                   "the owner re-armed it (count %d, contribution %d)",
                   old, k);
#else
  (void)call;
  (void)checked;
#endif
  if (old == k && fire != nullptr) CmiPushPE(L->ownerRank, fire);
}

// A local (source-completion) counter's contribution.
inline void contributeLocal(CmiCounter local, const char *call) {
  contribute(local->line, 1, call, false);
}

void rearmInternal(CmiCounterRec *c, int32_t n) {
  CounterLine *L = c->line;
  c->roundsArmed++;
  c->curN = n;
  c->sumArmed += (uint64_t)n;
  L->armedRounds.store(c->roundsArmed, std::memory_order_release);
  int32_t v = L->word.fetch_add(n, std::memory_order_acq_rel) + n;
  // the next round had already arrived in full (design 4.2); below zero,
  // more than that, and the action re-arms again to count the rest
  void *fire = L->fireMsg.load(std::memory_order_relaxed);
  if (v <= 0 && fire != nullptr) CmiPushPE(L->ownerRank, fire);
}

void runAction(CmiCounterRec *c, const CmiCounterAction &a, uint32_t round) {
  if (a.kind == CMI_ACTION_FN) {
    a.fn(a.arg, c);
  } else if (a.kind == CMI_ACTION_HANDLER) {
    auto *m = static_cast<CmiCounterFiredMsg *>(
        CmiAlloc(sizeof(CmiCounterFiredMsg)));
    m->counter = c;
    m->round = round;
    CmiSetHandler(m, a.handler);
    reinterpret_cast<CmiMessageHeader *>(m)->messageSize =
        sizeof(CmiCounterFiredMsg);
    CmiHandleMessage(m);
  }
}

// The counter's message: runs its action on the owner PE (design 5.3). A
// message whose counter was destroyed while it waited gives its index back.
void fireHandler(void *vmsg) {
  auto *m = static_cast<FireMsg *>(vmsg);
  PeState *ps = peState();
  CmiCounterRec *c =
      m->index < ps->counters.size() ? ps->counters[m->index] : nullptr;
  if (c == nullptr || c->generation != m->generation) {
    if (m->index < ps->withheld.size() && ps->withheld[m->index]) {
      ps->withheld[m->index] = 0;
      ps->freeCounters.push_back(m->index);
    }
    return;
  }
  c->roundsStarted++;
  c->sumStarted += (uint64_t)c->curN;
  qdCount(0, c->curN);
  uint32_t round = c->roundsStarted;
  CmiCounterAction a = c->action;
  // may queue m again, so m is not touched after this
  if (c->flags & CMI_COUNTER_AUTO_REARM) rearmInternal(c, c->curN);
  runAction(c, a, round);
}

void doneHandler(void *vmsg) {
  auto *m = static_cast<DoneMsg *>(vmsg);
  CmiDoneFn fn = m->fn;
  void *arg = m->arg;
  CmiFree(m);
  fn(arg);
}

void completeCounter(CmiCounterRec *c) {
  PeState *ps = peState();
  CounterLine *L = c->line;
  int32_t word = L->word.load(std::memory_order_acquire);
  bool hasAction = c->action.kind != CMI_ACTION_NONE;
  if (hasAction) {
    // contributions counted as created whose rounds QD has not seen consumed
    // (design 4.8): a partly filled round, or a fired one whose action had
    // not started
    int64_t landed = (int64_t)c->sumArmed - word;
    int64_t unconsumed = landed - (int64_t)c->sumStarted;
    if (unconsumed > 0) qdCount(0, (int)unconsumed);
  }
  // fired and queued (or about to be): the index waits for the message's
  // handler, so that no later counter there queues it a second time
  bool queued = hasAction && word <= 0 && c->roundsStarted < c->roundsArmed;
  L->flags.store(0, std::memory_order_release);
  L->fireMsg.store(nullptr, std::memory_order_relaxed);
  L->word.store(0, std::memory_order_relaxed);
  L->generation.fetch_add(1, std::memory_order_release);
  ps->counters[c->index] = nullptr;
  if (queued) {
    if (ps->withheld.size() <= c->index) ps->withheld.resize(c->index + 1, 0);
    ps->withheld[c->index] = 1;
  } else {
    ps->freeCounters.push_back(c->index);
  }
  postDone(c->done, c->doneArg);
  delete c;
}

// The contributions that have reached the counter's word (design 10.2).
int64_t landedOf(const CmiCounterRec *c) {
  return (int64_t)c->sumArmed -
         c->line->word.load(std::memory_order_acquire);
}

void scheduleRecheck(ObjKind kind, uint32_t slot, uint32_t gen, bool &flag);

// A destroy completes once every attached process has sent its notice and
// every contribution the notices report has landed; a counter's also waits
// for the regions it was paired with.
void tryCompleteCounter(CmiCounterRec *c) {
  if (c->state != ObjState::Destroying || !c->balance.empty() ||
      !c->pairedRegions.empty())
    return;
  if (landedOf(c) < (int64_t)c->expected) {
    scheduleRecheck(ObjKind::Counter, c->index, c->generation, c->recheck);
    return;
  }
  completeCounter(c);
}

void completeRegion(CmiRegionRec *r) {
  PeState *ps = peState();
  ps->regionGen[r->slot]++;
  // before the memory goes: an in-process put through a target of this
  // region now aborts
  regionSlotOf(ps->rank, r->slot)
      ->generation.store(ps->regionGen[r->slot], std::memory_order_release);
  ps->regions[r->slot] = nullptr;
  ps->freeRegions.push_back(r->slot);
  if (r->mr != comm_backend::MR_NULL) comm_backend::deregisterMemory(r->mr);
  if (r->runtimeAllocated) alignedFree(r->base);
  postDone(r->done, r->doneArg);
  // a counter's destroy waits for the regions it was paired with
  std::vector<CmiCounterRec *> paired;
  paired.swap(r->pairedCounters);
  for (CmiCounterRec *c : paired) {
    auto &v = c->pairedRegions;
    v.erase(std::remove(v.begin(), v.end(), r), v.end());
  }
  delete r;
  for (CmiCounterRec *c : paired) tryCompleteCounter(c);
}

// The region's puts have landed (design 10.2): those without a counter by
// the region's own count, those with one by their counters'.
void tryCompleteRegion(CmiRegionRec *r) {
  if (r->state != ObjState::Destroying || !r->balance.empty()) return;
  RegionSlot *rs = regionSlotOf(peState()->rank, r->slot);
  bool waiting = rs->landed.load(std::memory_order_acquire) < r->expectedUncounted;
  for (CmiCounterRec *c : r->pairedCounters) {
    // Exact for a counter paired with this region alone. For a counter
    // shared with another region in use or a target with no region, others'
    // contributions can make up for a put of this region still in flight;
    // correctness rests on the rule in CmiRegionDestroy's documentation.
    if (landedOf(c) < (int64_t)c->expected) waiting = true;
  }
  if (waiting) {
    scheduleRecheck(ObjKind::Region, r->slot, r->generation, r->recheck);
    return;
  }
  completeRegion(r);
}

// A destroy waiting for contributions in flight looks again shortly; they
// land at poll time, on threads that do not know about the destroy.
struct Recheck {
  ObjKind kind;
  uint32_t slot, gen;
};

void recheckFn(void *arg, double) {
  Recheck *rc = static_cast<Recheck *>(arg);
  PeState *ps = peState();
  if (rc->kind == ObjKind::Region) {
    if (rc->slot < ps->regions.size() && ps->regions[rc->slot] != nullptr &&
        ps->regions[rc->slot]->generation == rc->gen) {
      ps->regions[rc->slot]->recheck = false;
      tryCompleteRegion(ps->regions[rc->slot]);
    }
  } else if (rc->slot < ps->counters.size() && ps->counters[rc->slot] &&
             ps->counters[rc->slot]->generation == rc->gen) {
    ps->counters[rc->slot]->recheck = false;
    tryCompleteCounter(ps->counters[rc->slot]);
  }
  delete rc;
}

void scheduleRecheck(ObjKind kind, uint32_t slot, uint32_t gen, bool &flag) {
  if (flag) return;
  flag = true;
  CcdCallFnAfter(recheckFn, new Recheck{kind, slot, gen}, 0.05);
}

AttachEntry entryOf(const CmiRegionRec *r) {
  AttachEntry e{};
  e.ownerPe = r->ownerPe;
  e.slot = r->slot;
  e.gen = r->generation;
  e.kind = (uint8_t)ObjKind::Region;
  return e;
}

AttachEntry entryOf(const CmiCounterRec *c) {
  AttachEntry e{};
  e.ownerPe = c->ownerPe;
  e.slot = c->index;
  e.gen = c->generation;
  e.kind = (uint8_t)ObjKind::Counter;
  return e;
}

void requestDetachAll(const std::unordered_map<int, int> &balance,
                      const AttachEntry &e) {
  for (const auto &b : balance)
    if (b.second > 0) sendDetachRequest(b.first, e);
}

} // namespace

// ---------------------------------------------------------------------------
// The owner's side of the attach protocol (design 10.1)

namespace cmi_region {

bool applyAttach(AttachOp op, const AttachEntry &e, const CountEntry *counts,
                 int fromNode) {
  PeState *ps = peState();
  CmiRegionRec *r = nullptr;
  CmiCounterRec *c = nullptr;
  if (e.kind == (uint8_t)ObjKind::Region) {
    if (e.slot < ps->regions.size() && ps->regions[e.slot] != nullptr &&
        ps->regions[e.slot]->generation == e.gen)
      r = ps->regions[e.slot];
  } else if (e.slot < ps->counters.size() && ps->counters[e.slot] != nullptr &&
             ps->counters[e.slot]->generation == e.gen) {
    c = ps->counters[e.slot];
  }
  if (r == nullptr && c == nullptr) {
    // destroyed: a late registration is told to detach; a late notice dropped
    if (op == AttachOp::Register) sendDetachRequest(fromNode, e);
    return false;
  }
  // what the process contributed through this registration (10.2)
  std::vector<std::pair<uint32_t, uint32_t>> counted;
  if (op == AttachOp::Notice) {
    for (uint32_t i = 0; i < e.nCounts; i++) {
      const CountEntry &ce = counts[i];
      if (ce.counterIndex == kUncounted) {
        if (r) r->expectedUncounted += ce.count;
      } else if (ce.counterIndex < ps->counters.size() &&
                 ps->counters[ce.counterIndex] != nullptr &&
                 ps->counters[ce.counterIndex]->generation == ce.counterGen) {
        ps->counters[ce.counterIndex]->expected += ce.count;
        counted.push_back({ce.counterIndex, ce.counterGen});
      }
    }
  }
  std::unordered_map<int, int> &balance = r ? r->balance : c->balance;
  int b = balance[fromNode] += (op == AttachOp::Register ? 1 : -1);
  if (b == 0) balance.erase(fromNode);
  ObjState state = r ? r->state : c->state;
  int dflags = r ? r->destroyFlags : c->destroyFlags;
  if (op == AttachOp::Register && b > 0 && state == ObjState::Destroying &&
      (dflags & CMI_DESTROY_DETACH_ALL))
    sendDetachRequest(fromNode, e);
  if (r) tryCompleteRegion(r);
  else tryCompleteCounter(c);
  // a counter whose expected count rose may complete now; looked up again,
  // since the completions above can have freed it
  for (const auto &ig : counted)
    if (ig.first < ps->counters.size() && ps->counters[ig.first] != nullptr &&
        ps->counters[ig.first]->generation == ig.second)
      tryCompleteCounter(ps->counters[ig.first]);
  return true;
}

} // namespace cmi_region

// ---------------------------------------------------------------------------
// Arrivals from another process (design 5.2 (c), 5.3): only the decrement,
// and the push of the counter's message, run where the network is polled.

namespace {

// The counter of a contribution that arrived from another process, which the
// network or a message names by index; generation is checked when known.
CounterLine *arrivedLine(int rank, uint32_t index, const uint32_t *gen,
                         const char *call) {
  CounterLine *L = lineOf(rank, index);
  if (L == nullptr ||
      !(L->flags.load(std::memory_order_acquire) & kLineLive) ||
      (gen != nullptr && L->generation.load(std::memory_order_relaxed) != *gen))
    CmiAbort("%s: a contribution from another process reached counter %u "
             "(generation %u) of PE %d, which was destroyed; the target is "
             "stale. %s\n", call, index, gen ? *gen : 0u,
             CmiNodeFirst(CmiMyNode()) + rank, kReadinessAdvice);
  return L;
}

// Put with signal, kind kSignalCounter: the data is in place.
void counterSignalHandler(void *, int destRank, uint16_t signal, int, size_t) {
  contribute(arrivedLine(destRank, signal, nullptr, "CmiPut"), 1, "CmiPut");
}

// Put with signal, kind kSignalRegion: a put without a counter landed.
void regionSignalHandler(void *, int destRank, uint16_t signal, int, size_t) {
  RegionSlot *rs = regionSlotOf(destRank, signal);
  if (rs == nullptr)
    CmiAbort("CmiPut: a put from another process reached region slot %u of "
             "PE %d, which never existed\n", signal,
             CmiNodeFirst(CmiMyNode()) + destRank);
  rs->landed.fetch_add(1, std::memory_order_release);
  qdCount(0, 1);
}

// CmiSignal from another process: an active message, handled at poll time.
struct SignalMsg {
  char core[CmiMsgHeaderSizeBytes];
  int32_t rank;
  uint32_t index, gen;
};

void signalAmHandler(comm_backend::Status status) {
  auto *m = static_cast<SignalMsg *>(const_cast<void *>(status.local_buf));
  contribute(arrivedLine(m->rank, m->index, &m->gen, "CmiSignal"), 1,
             "CmiSignal");
  CmiFree(m);
}

// A put carried in a message, where the network has no put with signal, or
// for a small put where it is emulated. Handled on the owner PE: the copy
// runs there, not where the network is polled (W7).
struct PutDataMsg {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t pieceAddr, offset, bytes;
  uint32_t regionSlot, regionGen, counterIndex, counterGen;
  uint16_t flags;
  uint16_t pad;
  int32_t fromPe;
  // the data follows
};

CmiRegionRec *liveRegion(uint32_t slot, uint32_t gen, int fromPe,
                         const char *call) {
  PeState *ps = peState();
  if (slot >= ps->regions.size() || ps->regions[slot] == nullptr ||
      ps->regions[slot]->generation != gen)
    CmiAbort("%s: a transfer from PE %d reached region slot %u (generation "
             "%u) of PE %d, which was destroyed. %s\n", call, fromPe, slot,
             gen, CmiMyPe(), kReadinessAdvice);
  return ps->regions[slot];
}

void putDataHandler(void *vmsg) {
  auto *m = static_cast<PutDataMsg *>(vmsg);
  liveRegion(m->regionSlot, m->regionGen, m->fromPe, "CmiPut");
  memcpy((char *)(uintptr_t)m->pieceAddr + m->offset, m + 1, m->bytes);
  if (m->flags & kTargetHasCounter) {
    contribute(arrivedLine(peState()->rank, m->counterIndex, &m->counterGen,
                           "CmiPut"),
               1, "CmiPut");
  } else {
    regionSlotOf(peState()->rank, m->regionSlot)
        ->landed.fetch_add(1, std::memory_order_release);
    qdCount(0, 1);
  }
  CmiFree(m);
}

// A get without one-sided support: a request to the owner PE, which answers
// with the bytes.
struct GetCtx {
  CmiAttachRec *rec;
  CmiCounter local;
  void *dst;
};
struct GetRequestMsg {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t src, bytes;
  uint32_t regionSlot, regionGen;
  int32_t replyPe;
  GetCtx *ctx;
};
struct GetReplyMsg {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t bytes;
  GetCtx *ctx;
  // the data follows
};

void getRequestHandler(void *vmsg) {
  auto *m = static_cast<GetRequestMsg *>(vmsg);
  liveRegion(m->regionSlot, m->regionGen, m->replyPe, "CmiGet");
  int size = (int)(sizeof(GetReplyMsg) + m->bytes);
  auto *r = static_cast<GetReplyMsg *>(CmiAlloc(size));
  r->bytes = m->bytes;
  r->ctx = m->ctx;
  memcpy(r + 1, (const void *)(uintptr_t)m->src, m->bytes);
  CmiSetHandler(r, CpvAccess(regionGetReplyHandlerIdx));
  CmiSyncSendAndFree(m->replyPe, size, r);
  CmiFree(m);
}

// A get has completed at the reader: by the network, or by the reply.
void getDone(GetCtx *ctx) {
  if (ctx->local != nullptr) contributeLocal(ctx->local, "CmiGet");
  qdCount(0, 1);
  operationDone(ctx->rec);
  delete ctx;
}

void getReplyHandler(void *vmsg) {
  auto *m = static_cast<GetReplyMsg *>(vmsg);
  memcpy(m->ctx->dst, m + 1, m->bytes);
  getDone(m->ctx);
  CmiFree(m);
}

void getLocalDone(comm_backend::Status status) {
  getDone(static_cast<GetCtx *>(status.user_context));
}

// A put with signal's source may be reused (design 4.5).
struct PutCtx {
  CmiAttachRec *rec;
  CmiCounter local;
};

void putLocalDone(comm_backend::Status status) {
  auto *ctx = static_cast<PutCtx *>(status.user_context);
  if (ctx->local != nullptr) contributeLocal(ctx->local, "CmiPut");
  operationDone(ctx->rec);
  delete ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// Initialisation

void CmiRegionNodeInit(char **argv) {
  int limit = (int)kMaxCountersPerPe;
  if (CmiGetArgIntDesc(argv, "+counter_table", &limit,
                       "counters a PE may have at once (at most 65536)")) {
    if (limit < 1 || limit > (int)kMaxCountersPerPe)
      CmiAbort("+counter_table %d: the limit is between 1 and %u\n", limit,
               kMaxCountersPerPe);
  }
  g_counterLimit = (uint32_t)limit;
  size_t page = std::max<size_t>(CmiGetPageSize(), CMI_CACHE_LINE_SIZE);
  g_linesPerChunk = (uint32_t)(page / sizeof(CounterLine));
  uint32_t nchunks =
      (kMaxCountersPerPe + g_linesPerChunk - 1) / g_linesPerChunk;
  g_slotsPerChunk = (uint32_t)(page / sizeof(RegionSlot));
  uint32_t nslotChunks =
      (kMaxRegionsPerPe + g_slotsPerChunk - 1) / g_slotsPerChunk;
  int nranks = CmiMyNodeSize();
  g_slices = new CounterSlice[nranks];
  for (int r = 0; r < nranks; r++) {
    g_slices[r].chunks = new std::atomic<CounterLine *>[nchunks];
    for (uint32_t i = 0; i < nchunks; i++)
      g_slices[r].chunks[i].store(nullptr, std::memory_order_relaxed);
    g_slices[r].slotChunks = new std::atomic<RegionSlot *>[nslotChunks];
    for (uint32_t i = 0; i < nslotChunks; i++)
      g_slices[r].slotChunks[i].store(nullptr, std::memory_order_relaxed);
  }
  attachNodeInit();

  // Between processes: regions are registered for one-sided transfers when
  // the backend has them (RDMAInit has read +nordma), and a put carries its
  // signal in the network operation where the backend can.
  g_useRma = comm_backend::isRMACapable() && !CmiUseCopyBasedRDMA;
  g_usePutSignal = g_useRma && comm_backend::isPutSignalCapable();
  if (g_usePutSignal) {
    // the same kinds in the same order in every process, before the
    // start-up barrier that precedes any put
    g_putKinds[kSignalCounter] =
        comm_backend::registerPutSignalKind(counterSignalHandler, nullptr, nranks);
    g_putKinds[kSignalRegion] =
        comm_backend::registerPutSignalKind(regionSignalHandler, nullptr, nranks);
  }
  if (g_useRma &&
      comm_backend::getRMR(comm_backend::MR_NULL, nullptr, 0) >
          CMI_TARGET_RKEY_BYTES)
    CmiAbort("regions: the backend's remote key takes %zu bytes, more than a "
             "target's %d\n",
             comm_backend::getRMR(comm_backend::MR_NULL, nullptr, 0),
             CMI_TARGET_RKEY_BYTES);
  // Where the backend emulates the put with signal (the write, its delivery
  // completion, then a message), a put that fits one eager message is
  // quicker as that message.
  int messageMax = comm_backend::isPutSignalNative()
                       ? 0
                       : (int)comm_backend::getMaxEagerBytes();
  CmiGetArgIntDesc(argv, "+put_message_max_bytes", &messageMax,
                   "between processes, a CmiPut of at most this many bytes "
                   "travels as one message with its signal (default 0 where "
                   "the put with signal is one network operation, else the "
                   "backend's eager limit)");
  if (messageMax < 0) messageMax = 0;
  g_putMessageMax = (size_t)messageMax;
  g_verbose = CmiGetArgFlagDesc(argv, "+region_verbose",
                                "say how a CmiPut to another process travels, "
                                "the first time each way is used");
  g_signalAm = comm_backend::registerAmHandler(signalAmHandler);
#if CMI_REGION_CHECKS
  CmiGetArgIntDesc(argv, "+region_test_completion_delay", &g_completionDelayUs,
                   "error-checking builds, for tests: microseconds a transfer "
                   "to another process waits in its completion before it is "
                   "counted, which widens the window a detach can race");
#endif
}

void CmiRegionPeInit(void) {
  CpvInitialize(PeState *, regionPeState);
  CpvAccess(regionPeState) = new PeState();
  CpvAccess(regionPeState)->rank = CmiMyRank();
  CpvInitialize(int, regionFireHandlerIdx);
  CpvAccess(regionFireHandlerIdx) = CmiRegisterHandler(fireHandler);
  CpvInitialize(int, regionDoneHandlerIdx);
  CpvAccess(regionDoneHandlerIdx) = CmiRegisterHandler(doneHandler);
  CpvInitialize(int, regionPutDataHandlerIdx);
  CpvAccess(regionPutDataHandlerIdx) = CmiRegisterHandler(putDataHandler);
  CpvInitialize(int, regionGetRequestHandlerIdx);
  CpvAccess(regionGetRequestHandlerIdx) = CmiRegisterHandler(getRequestHandler);
  CpvInitialize(int, regionGetReplyHandlerIdx);
  CpvAccess(regionGetReplyHandlerIdx) = CmiRegisterHandler(getReplyHandler);
  attachPeInit();
}

size_t CmiRegionPutMessageMaxBytes(void) { return g_putMessageMax; }

void CmiRegionSetQdHook(CmiRegionQdFn fn) {
  g_qdHook.store(fn, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Counters (design 3.2, 4.2)

CmiCounter CmiCounterCreate(int32_t n, CmiCounterAction action, int flags) {
  if (n < 1)
    CmiAbort("CmiCounterCreate: N is %d; a round needs at least one "
             "contribution\n", (int)n);
  switch (action.kind) {
  case CMI_ACTION_NONE:
    if (flags & CMI_COUNTER_AUTO_REARM)
      CmiAbort("CmiCounterCreate: CMI_COUNTER_AUTO_REARM needs an action, "
               "which re-arms the counter as it starts\n");
    break;
  case CMI_ACTION_FN:
    if (action.fn == nullptr)
      CmiAbort("CmiCounterCreate: a function action with no function\n");
    break;
  case CMI_ACTION_HANDLER:
    if (action.handler < 0)
      CmiAbort("CmiCounterCreate: a handler action with handler %d\n",
               action.handler);
    break;
  default:
    CmiAbort("CmiCounterCreate: unknown action kind %d\n", (int)action.kind);
  }
  PeState *ps = peState();
  uint32_t index;
  if (!ps->freeCounters.empty()) {
    index = ps->freeCounters.front();
    ps->freeCounters.pop_front();
  } else {
    if (ps->nextCounter >= g_counterLimit)
      CmiAbort("CmiCounterCreate: PE %d already has %u counters, the limit "
               "(+counter_table, at most 65536)\n", CmiMyPe(), g_counterLimit);
    index = ps->nextCounter++;
  }
  CounterLine *L = allocLine(ps, index);

  auto *c = new CmiCounterRec();
  c->line = L;
  c->ownerPe = CmiMyPe();
  c->index = index;
  c->generation = L->generation.load(std::memory_order_relaxed);
  c->action = action;
  c->flags = flags;
  c->curN = n;
  c->roundsStarted = 0;
  c->roundsArmed = 1;
  c->sumArmed = (uint64_t)n;
  c->sumStarted = 0;
  c->expected = 0;
  c->state = ObjState::Live;
  c->destroyFlags = 0;
  c->recheck = false;
  c->done = nullptr;
  c->doneArg = nullptr;

  FireMsg *fire = nullptr;
  if (action.kind != CMI_ACTION_NONE) {
    if (ps->messages.size() <= index) ps->messages.resize(index + 1, nullptr);
    fire = ps->messages[index];
    if (fire == nullptr) {
      fire = static_cast<FireMsg *>(CmiAlloc(sizeof(FireMsg)));
      fire->rank = ps->rank;
      fire->index = index;
      CmiSetHandler(fire, CpvAccess(regionFireHandlerIdx));
      reinterpret_cast<CmiMessageHeader *>(fire)->messageSize = sizeof(FireMsg);
      ps->messages[index] = fire;
    }
    fire->generation = c->generation;
  }
  L->word.store(n, std::memory_order_relaxed);
  L->armedRounds.store(1, std::memory_order_relaxed);
  L->ownerRank = ps->rank;
  L->index = index;
  L->fireMsg.store(fire, std::memory_order_relaxed);
  L->flags.store(kLineLive |
                     ((flags & CMI_COUNTER_CHECK_ROUNDS) ? kLineCheckRounds : 0),
                 std::memory_order_release);
  if (ps->counters.size() <= index) ps->counters.resize(index + 1, nullptr);
  ps->counters[index] = c;
  return c;
}

void CmiCounterRearm(CmiCounter c, int32_t n) {
  checkOwner(c->ownerPe, "CmiCounterRearm", "counter");
  if (c->state != ObjState::Live)
    CmiAbort("CmiCounterRearm: counter %u of PE %d is being destroyed\n",
             c->index, c->ownerPe);
  if (c->flags & CMI_COUNTER_AUTO_REARM)
    CmiAbort("CmiCounterRearm: counter %u of PE %d re-arms itself "
             "(CMI_COUNTER_AUTO_REARM)\n", c->index, c->ownerPe);
  if (n < 1)
    CmiAbort("CmiCounterRearm: N is %d; a round needs at least one "
             "contribution\n", (int)n);
  if (c->action.kind == CMI_ACTION_NONE) {
    int32_t v = c->line->word.load(std::memory_order_acquire);
    if (v > 0)
      CmiAbort("CmiCounterRearm: counter %u (generation %u) of PE %d, round "
               "%u: re-armed while %d contributions of the round are still "
               "expected\n", c->index, c->generation, c->ownerPe,
               c->roundsArmed, (int)v);
    c->roundsStarted = c->roundsArmed;
    c->sumStarted = c->sumArmed;
  } else if (c->roundsStarted != c->roundsArmed) {
    // the round's message would be queued a second time (design 4.2)
    CmiAbort("CmiCounterRearm: counter %u (generation %u) of PE %d, round %u: "
             "re-armed before the round's action started; re-arm in the "
             "action or after it\n", c->index, c->generation, c->ownerPe,
             c->roundsArmed);
  }
  rearmInternal(c, n);
}

int32_t CmiCounterValue(CmiCounter c) {
  return c->line->word.load(std::memory_order_acquire);
}

void CmiCounterDestroy(CmiCounter c, int flags, CmiDoneFn done, void *arg) {
  checkOwner(c->ownerPe, "CmiCounterDestroy", "counter");
  if (c->state != ObjState::Live)
    CmiAbort("CmiCounterDestroy: counter %u of PE %d is already being "
             "destroyed\n", c->index, c->ownerPe);
  c->state = ObjState::Destroying;
  c->destroyFlags = flags;
  c->done = done;
  c->doneArg = arg;
  if (flags & CMI_DESTROY_DETACH_ALL) requestDetachAll(c->balance, entryOf(c));
  tryCompleteCounter(c);
}

// ---------------------------------------------------------------------------
// Regions

CmiRegion CmiRegionCreate(void *base, size_t bytes, CmiMemKind kind, int devId,
                          int flags) {
  (void)devId;
  (void)flags;
  if (kind != CMI_MEM_HOST)
    CmiAbort("CmiRegionCreate: device regions are not implemented yet\n");
  if (base == nullptr || bytes == 0)
    CmiAbort("CmiRegionCreate: a region needs memory (base %p, %zu bytes)\n",
             base, bytes);
  PeState *ps = peState();
  uint32_t slot;
  if (!ps->freeRegions.empty()) {
    slot = ps->freeRegions.front();
    ps->freeRegions.pop_front();
  } else {
    if (ps->regions.size() >= kMaxRegionsPerPe)
      CmiAbort("CmiRegionCreate: PE %d already has %u regions, the limit\n",
               CmiMyPe(), kMaxRegionsPerPe);
    slot = (uint32_t)ps->regions.size();
    ps->regions.push_back(nullptr);
    ps->regionGen.push_back(0);
  }
  // the previous region in the slot completed its destroy only once its
  // puts had landed; the generation is already this region's
  RegionSlot *rs = allocSlot(ps, slot);
  rs->landed.store(0, std::memory_order_relaxed);
  rs->generation.store(ps->regionGen[slot], std::memory_order_release);
  auto *r = new CmiRegionRec();
  r->base = base;
  r->bytes = bytes;
  r->kind = kind;
  r->runtimeAllocated = false;
  r->ownerPe = CmiMyPe();
  r->slot = slot;
  r->generation = ps->regionGen[slot];
  r->mr = comm_backend::MR_NULL;
  memset(r->rkey, 0, sizeof(r->rkey));
  if (g_useRma && CmiNumNodes() > 1) {
    // registered once, on this PE's network device (design 6)
    r->mr = comm_backend::registerMemory(base, bytes);
    comm_backend::getRMR(r->mr, r->rkey, sizeof(r->rkey));
  }
  r->expectedUncounted = 0;
  r->state = ObjState::Live;
  r->destroyFlags = 0;
  r->recheck = false;
  r->done = nullptr;
  r->doneArg = nullptr;
  ps->regions[slot] = r;
  return r;
}

void *CmiRegionAlloc(size_t bytes, CmiMemKind kind, int devId, int flags,
                     CmiRegion *out) {
  if (kind != CMI_MEM_HOST)
    CmiAbort("CmiRegionAlloc: device regions are not implemented yet\n");
  if (bytes == 0) CmiAbort("CmiRegionAlloc: a region of 0 bytes\n");
  void *base = alignedAlloc(CMI_CACHE_LINE_SIZE, bytes, "CmiRegionAlloc");
  CmiRegion r = CmiRegionCreate(base, bytes, kind, devId, flags);
  r->runtimeAllocated = true;
  if (out != nullptr) *out = r;
  return base;
}

int CmiRegionAddPiece(CmiRegion r, size_t offset, size_t bytes,
                      const CmiLayout *layout, int flags) {
  checkOwner(r->ownerPe, "CmiRegionAddPiece", "region");
  if (r->state != ObjState::Live)
    CmiAbort("CmiRegionAddPiece: the region is being destroyed\n");
  if (layout != nullptr)
    CmiAbort("CmiRegionAddPiece: strided pieces (a layout) are not "
             "implemented yet\n");
  if (flags & CMI_PIECE_VARSIZE)
    CmiAbort("CmiRegionAddPiece: variable-size pieces (CMI_PIECE_VARSIZE) "
             "are not implemented yet\n");
  if (bytes == 0 || offset > r->bytes || bytes > r->bytes - offset)
    CmiAbort("CmiRegionAddPiece: a piece of %zu bytes at offset %zu does not "
             "fit in a region of %zu bytes\n", bytes, offset, r->bytes);
  r->pieces.push_back({offset, bytes});
  return (int)r->pieces.size() - 1;
}

size_t CmiRegionPieceLength(CmiRegion r, int piece) {
  (void)r;
  (void)piece;
  CmiAbort("CmiRegionPieceLength: variable-size pieces are not implemented "
           "yet\n");
}

void CmiRegionDestroy(CmiRegion r, int flags, CmiDoneFn done, void *arg) {
  checkOwner(r->ownerPe, "CmiRegionDestroy", "region");
  if (r->state != ObjState::Live)
    CmiAbort("CmiRegionDestroy: the region is already being destroyed\n");
  r->state = ObjState::Destroying;
  r->destroyFlags = flags;
  r->done = done;
  r->doneArg = arg;
  if (flags & CMI_DESTROY_DETACH_ALL) requestDetachAll(r->balance, entryOf(r));
  tryCompleteRegion(r);
}

// ---------------------------------------------------------------------------
// Targets

CmiTarget CmiTargetMake(CmiRegion r, int piece, CmiCounter c) {
  if (r == nullptr && c == nullptr)
    CmiAbort("CmiTargetMake: a target needs a region, a counter or both\n");
  if (r != nullptr) checkOwner(r->ownerPe, "CmiTargetMake", "region");
  if (c != nullptr) checkOwner(c->ownerPe, "CmiTargetMake", "counter");
  if ((r && r->state != ObjState::Live) || (c && c->state != ObjState::Live))
    CmiAbort("CmiTargetMake: the %s is being destroyed\n",
             (r && r->state != ObjState::Live) ? "region" : "counter");
  CmiTarget t;
  memset(&t, 0, sizeof(t));
  t.magic = CMI_TARGET_MAGIC;
  t.ownerNode = CmiMyNode();
  t.ownerPe = CmiMyPe();
  if (r != nullptr) {
    size_t offset = 0, bytes = r->bytes;
    if (piece != CMI_PIECE_ALL) {
      if (piece < 0 || piece >= (int)r->pieces.size())
        CmiAbort("CmiTargetMake: piece %d of a region with %zu pieces\n",
                 piece, r->pieces.size());
      offset = r->pieces[piece].offset;
      bytes = r->pieces[piece].bytes;
    }
    t.flags |= kTargetHasRegion;
    t.memKind = (uint16_t)r->kind;
    t.regionSlot = r->slot;
    t.regionGen = r->generation;
    t.pieceAddr = (uint64_t)(uintptr_t)((char *)r->base + offset);
    t.pieceDisp = offset;
    t.pieceBytes = bytes;
    if (r->mr != comm_backend::MR_NULL) {
      t.flags |= kTargetHasRkey;
      memcpy(t.rkey, r->rkey, sizeof(t.rkey));
    }
    t.sigKind = kSignalRegion;
    t.sigValue = (uint16_t)r->slot;
  }
  if (c != nullptr) {
    t.flags |= kTargetHasCounter;
    if (c->action.kind != CMI_ACTION_NONE) t.flags |= kTargetQdCounted;
    t.counterIndex = c->index;
    t.counterGen = c->generation;
    t.sigKind = kSignalCounter;
    t.sigValue = (uint16_t)c->index;
  }
  if (r != nullptr && c != nullptr &&
      std::find(r->pairedCounters.begin(), r->pairedCounters.end(), c) ==
          r->pairedCounters.end()) {
    r->pairedCounters.push_back(c);
    c->pairedRegions.push_back(r);
  }
  return t;
}

// ---------------------------------------------------------------------------
// The data paths (design 5.2, 6)

namespace {

CmiAttachmentRec *checkHandle(CmiAttachment a, const char *call) {
  if (a == nullptr || a->magic != CMI_TARGET_MAGIC)
    CmiAbort("%s: not an attachment from CmiAttachTarget, or one already "
             "detached\n", call);
  return a;
}

[[noreturn]] void abortDetachedByOwner(const CmiAttachmentRec *a,
                                       const char *call) {
  CmiAbort("%s: the owner, PE %d, destroyed this target's %s and detached "
           "this process. %s\n", call, a->target.ownerPe,
           (a->target.flags & kTargetHasRegion) ? "region" : "counter",
           kReadinessAdvice);
}

// Within the owner's process: a registration the owner ended (its destroy
// with CMI_DESTROY_DETACH_ALL, or an attach to an object already destroyed)
// is refused in every build (design 11). A destroy that completes between
// this check and the copy is the application's error, and is not caught.
void checkLocalState(CmiAttachmentRec *a, const char *call) {
  if (a->rec->state.load(std::memory_order_acquire) ==
      (int)AttachState::DetachedByOwner)
    abortDetachedByOwner(a, call);
}

// To another process: the transfer is counted in flight before it looks at
// the registration's state, and the owner's detach request sets the state
// before it looks at the count (detachRequestHandler, oweNotice), all
// seq_cst. Either the detach sees this transfer and its notice waits for
// it, or this transfer sees the detach and aborts, in every build, without
// issuing anything. So no transfer is counted after the notice is sent, and
// only one completion can see the notice waiting. The caller ends the
// transfer with operationDone: at its local completion, or right after the
// send for one that a message carries.
CmiAttachRec *beginRemote(CmiAttachmentRec *a, const char *call) {
  CmiAttachRec *rec = a->rec;
  rec->outstanding.fetch_add(1, std::memory_order_seq_cst);
  if (rec->state.load(std::memory_order_seq_cst) ==
      (int)AttachState::DetachedByOwner)
    abortDetachedByOwner(a, call);
  return rec;
}

bool isLocal(const CmiTarget &t) { return t.ownerNode == CmiMyNode(); }

// One more contribution through this attachment, for its detach notice
// (design 10.2); returns how many there have been.
uint64_t counted(CmiAttachmentRec *a) {
  return a->contributions.fetch_add(1, std::memory_order_relaxed) + 1;
}

// The target's counter, in this process, checked against the target's
// generation in every build: the check reads the line the contribution
// writes next. k is this contribution's number through the attachment.
CounterLine *liveLine(CmiAttachmentRec *a, uint64_t k, const char *call) {
  const CmiTarget &t = a->target;
  CounterLine *L = lineOf(CmiRankOf(t.ownerPe), t.counterIndex);
  if (L == nullptr ||
      !(L->flags.load(std::memory_order_acquire) & kLineLive) ||
      L->generation.load(std::memory_order_relaxed) != t.counterGen)
    CmiAbort("%s: counter %u (generation %u) of PE %d was destroyed; this "
             "target is stale. %s\n", call, t.counterIndex, t.counterGen,
             t.ownerPe, kReadinessAdvice);
#if CMI_REGION_CHECKS
  // the k-th contribution through one attachment needs round k armed
  if (L->flags.load(std::memory_order_relaxed) & kLineCheckRounds) {
    uint32_t armed = L->armedRounds.load(std::memory_order_acquire);
    if (k > armed)
      abortReadiness(call, L,
                     "this attachment's contribution %llu arrived while the "
                     "owner had armed %u rounds, so it would count in place "
                     "of another origin's", (unsigned long long)k, armed);
  }
#else
  (void)k;
#endif
  return L;
}

// The target's region, in this process, checked against the target's
// generation in every build: one load of the slot's generation, which the
// destroy changes before it deregisters or frees the memory. A destroy that
// completes between this check and the copy is the application's error.
void liveRegionHere(const CmiTarget &t, const char *call) {
  RegionSlot *rs = regionSlotOf(CmiRankOf(t.ownerPe), t.regionSlot);
  if (rs == nullptr ||
      rs->generation.load(std::memory_order_acquire) != t.regionGen)
    CmiAbort("%s: region slot %u (generation %u) of PE %d was destroyed; "
             "this target is stale. %s\n", call, t.regionSlot, t.regionGen,
             t.ownerPe, kReadinessAdvice);
}

// QD (design 4.8): a contribution to a counter with an action is processed
// with its round; a put without a counter, when it lands.
void qdIssued(const CmiTarget &t, bool contributes, CmiCounter local) {
  if (contributes && (t.flags & kTargetHasCounter
                          ? (t.flags & kTargetQdCounted) != 0
                          : !isLocal(t)))
    qdCount(1, 0);
  if (local != nullptr && local->action.kind != CMI_ACTION_NONE) qdCount(1, 0);
}

void showPath(int path, size_t bytes, const CmiTarget &t) {
  if (!g_verbose || g_pathShown[path].exchange(true)) return;
  CmiPrintf("Reconverse> regions: a %zu-byte CmiPut to process %d travels %s\n",
            bytes, t.ownerNode,
            path == 0 ? "as a put with signal"
                      : "in one message with its signal");
}

void putRemote(CmiAttachmentRec *a, CmiAttachRec *rec, size_t offset,
               const void *src, size_t bytes, CmiCounter local) {
  const CmiTarget &t = a->target;
  if (g_usePutSignal && (t.flags & kTargetHasRkey) && bytes > g_putMessageMax) {
    showPath(0, bytes, t);
    comm_backend::issueRputSignal(
        CmiNodeToGlobal(t.ownerNode), src, bytes, comm_backend::MR_NULL,
        (uintptr_t)(t.pieceDisp + offset), const_cast<uint8_t *>(t.rkey),
        CmiRankOf(t.ownerPe), g_putKinds[t.sigKind], t.sigValue, putLocalDone,
        new PutCtx{rec, local});
    return;
  }
  showPath(1, bytes, t);
  int size = (int)(sizeof(PutDataMsg) + bytes);
  auto *m = static_cast<PutDataMsg *>(CmiAlloc(size));
  m->pieceAddr = t.pieceAddr;
  m->offset = offset;
  m->bytes = bytes;
  m->regionSlot = t.regionSlot;
  m->regionGen = t.regionGen;
  m->counterIndex = t.counterIndex;
  m->counterGen = t.counterGen;
  m->flags = t.flags;
  m->pad = 0;
  m->fromPe = CmiMyPe();
  memcpy(m + 1, src, bytes);
  CmiSetHandler(m, CpvAccess(regionPutDataHandlerIdx));
  CmiSyncSendAndFree(t.ownerPe, size, m);
  if (local != nullptr) contributeLocal(local, "CmiPut");
  operationDone(rec);
}

} // namespace

int CmiPut(CmiAttachment t, size_t offset, const void *src, size_t bytes,
           const CmiLayout *srcLayout, CmiCounter local, void *stream) {
  CmiAttachmentRec *a = checkHandle(t, "CmiPut");
  const CmiTarget &tg = a->target;
  if (!(tg.flags & kTargetHasRegion)) return CMI_REGION_ENOREGION;
  if (srcLayout != nullptr || stream != nullptr) return CMI_REGION_EUNSUPPORTED;
  if (offset > tg.pieceBytes || bytes > tg.pieceBytes - offset)
    return CMI_REGION_ECAPACITY;
  bool hasCounter = (tg.flags & kTargetHasCounter) != 0;
  if (!isLocal(tg)) {
    CmiAttachRec *rec = beginRemote(a, "CmiPut");
    counted(a);
    qdIssued(tg, true, local);
    putRemote(a, rec, offset, src, bytes, local);
    return CMI_REGION_OK;
  }
  checkLocalState(a, "CmiPut");
  liveRegionHere(tg, "CmiPut");
  CounterLine *L = hasCounter ? liveLine(a, counted(a), "CmiPut") : nullptr;
  qdIssued(tg, hasCounter, local);
  memcpy((char *)(uintptr_t)tg.pieceAddr + offset, src, bytes);
  if (L != nullptr) contribute(L, 1, "CmiPut");
  if (local != nullptr) contributeLocal(local, "CmiPut");
  return CMI_REGION_OK;
}

int CmiGet(void *dst, size_t bytes, const CmiLayout *dstLayout,
           CmiAttachment t, size_t offset, CmiCounter local, void *stream) {
  CmiAttachmentRec *a = checkHandle(t, "CmiGet");
  const CmiTarget &tg = a->target;
  if (!(tg.flags & kTargetHasRegion)) return CMI_REGION_ENOREGION;
  if (dstLayout != nullptr || stream != nullptr) return CMI_REGION_EUNSUPPORTED;
  if (offset > tg.pieceBytes || bytes > tg.pieceBytes - offset)
    return CMI_REGION_ECAPACITY;
  if (isLocal(tg)) {
    checkLocalState(a, "CmiGet");
    liveRegionHere(tg, "CmiGet");
    memcpy(dst, (const char *)(uintptr_t)tg.pieceAddr + offset, bytes);
    qdIssued(tg, false, local);
    if (local != nullptr) contributeLocal(local, "CmiGet");
    return CMI_REGION_OK;
  }
  CmiAttachRec *rec = beginRemote(a, "CmiGet");
  // created here, processed at its completion (design 4.8)
  qdCount(1, 0);
  qdIssued(tg, false, local);
  auto *ctx = new GetCtx{rec, local, dst};
  if (g_useRma && (tg.flags & kTargetHasRkey) && bytes > 0) {
    comm_backend::issueRget(CmiNodeToGlobal(tg.ownerNode), dst, bytes,
                            comm_backend::MR_NULL,
                            (void *)(uintptr_t)(tg.pieceAddr + offset),
                            const_cast<uint8_t *>(tg.rkey), getLocalDone, ctx);
    return CMI_REGION_OK;
  }
  auto *m = static_cast<GetRequestMsg *>(CmiAlloc(sizeof(GetRequestMsg)));
  m->src = tg.pieceAddr + offset;
  m->bytes = bytes;
  m->regionSlot = tg.regionSlot;
  m->regionGen = tg.regionGen;
  m->replyPe = CmiMyPe();
  m->ctx = ctx;
  CmiSetHandler(m, CpvAccess(regionGetRequestHandlerIdx));
  CmiSyncSendAndFree(tg.ownerPe, sizeof(GetRequestMsg), m);
  return CMI_REGION_OK;
}

int CmiSignal(CmiAttachment t) {
  CmiAttachmentRec *a = checkHandle(t, "CmiSignal");
  const CmiTarget &tg = a->target;
  if (!(tg.flags & kTargetHasCounter)) return CMI_REGION_ENOCOUNTER;
  if (!isLocal(tg)) {
    CmiAttachRec *rec = beginRemote(a, "CmiSignal");
    counted(a);
    qdIssued(tg, true, nullptr);
    auto *m = static_cast<SignalMsg *>(CmiAlloc(sizeof(SignalMsg)));
    m->rank = CmiRankOf(tg.ownerPe);
    m->index = tg.counterIndex;
    m->gen = tg.counterGen;
    reinterpret_cast<CmiMessageHeader *>(m)->messageSize = sizeof(SignalMsg);
    comm_backend::issueAm(CmiNodeToGlobal(tg.ownerNode), m, sizeof(SignalMsg),
                          MRFIELD(m), CommLocalHandler, g_signalAm, nullptr);
    operationDone(rec);
    return CMI_REGION_OK;
  }
  checkLocalState(a, "CmiSignal");
  CounterLine *L = liveLine(a, counted(a), "CmiSignal");
  qdIssued(tg, true, nullptr);
  contribute(L, 1, "CmiSignal");
  return CMI_REGION_OK;
}
