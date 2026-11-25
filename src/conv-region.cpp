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

#include "conv-region-internal.h"

using namespace cmi_region;

namespace {

// The sentence design 11 ends every readiness abort with.
const char kReadinessAdvice[] =
    "If the application's logic cannot guarantee this transfer's readiness, "
    "use a message send here.";

CounterSlice *g_slices = nullptr;
uint32_t g_linesPerChunk = 0;
uint32_t g_counterLimit = kMaxCountersPerPe;
std::atomic<CmiRegionQdFn> g_qdHook{nullptr};

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

void qdCount(int created, int processed) {
  CmiRegionQdFn fn = g_qdHook.load(std::memory_order_acquire);
  if (fn != nullptr) fn(created, processed);
}

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
// decrement cannot hand this thread another counter's message.
inline void contribute(CounterLine *L, int32_t k, const char *call) {
  void *fire = L->fireMsg.load(std::memory_order_relaxed);
  int32_t old = L->word.fetch_sub(k, std::memory_order_acq_rel);
#if CMI_REGION_CHECKS
  if (old < k)
    abortReadiness(call, L,
                   "a contribution arrived after the round fired and before "
                   "the owner re-armed it (count %d, contribution %d)",
                   old, k);
#else
  (void)call;
#endif
  if (old == k && fire != nullptr) CmiPushPE(L->ownerRank, fire);
}

void rearmInternal(CmiCounterRec *c, int32_t n) {
  CounterLine *L = c->line;
  c->roundsArmed++;
  c->curN = n;
  c->sumArmed += (uint64_t)n;
  L->armedRounds.store(c->roundsArmed, std::memory_order_release);
  int32_t v = L->word.fetch_add(n, std::memory_order_acq_rel) + n;
  // the next round had already arrived in full (design 4.2)
  void *fire = L->fireMsg.load(std::memory_order_relaxed);
  if (v == 0 && fire != nullptr) CmiPushPE(L->ownerRank, fire);
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
// message whose counter was destroyed while it waited is freed here.
void fireHandler(void *vmsg) {
  auto *m = static_cast<FireMsg *>(vmsg);
  PeState *ps = peState();
  CmiCounterRec *c =
      m->index < ps->counters.size() ? ps->counters[m->index] : nullptr;
  if (c == nullptr || c->generation != m->generation) {
    CmiFree(m);
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
  // fired and queued: fireHandler frees the message when it finds the
  // counter gone
  bool queued = hasAction && word <= 0 && c->roundsStarted < c->roundsArmed;
  void *fire = L->fireMsg.load(std::memory_order_relaxed);
  L->flags.store(0, std::memory_order_release);
  L->fireMsg.store(nullptr, std::memory_order_relaxed);
  L->word.store(0, std::memory_order_relaxed);
  L->generation.fetch_add(1, std::memory_order_release);
  ps->counters[c->index] = nullptr;
  ps->freeCounters.push_back(c->index);
  if (fire != nullptr && !queued) CmiFree(fire);
  postDone(c->done, c->doneArg);
  delete c;
}

void tryCompleteCounter(CmiCounterRec *c) {
  if (c->state != ObjState::Destroying || !c->balance.empty() ||
      !c->pairedRegions.empty())
    return;
  completeCounter(c);
}

void completeRegion(CmiRegionRec *r) {
  PeState *ps = peState();
  ps->regionGen[r->slot]++;
  ps->regions[r->slot] = nullptr;
  ps->freeRegions.push_back(r->slot);
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

void tryCompleteRegion(CmiRegionRec *r) {
  if (r->state != ObjState::Destroying || !r->balance.empty()) return;
  completeRegion(r);
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

bool applyAttach(AttachOp op, const AttachEntry &e, int fromNode) {
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
  return true;
}

} // namespace cmi_region

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
  int nranks = CmiMyNodeSize();
  g_slices = new CounterSlice[nranks];
  for (int r = 0; r < nranks; r++) {
    g_slices[r].chunks = new std::atomic<CounterLine *>[nchunks];
    for (uint32_t i = 0; i < nchunks; i++)
      g_slices[r].chunks[i].store(nullptr, std::memory_order_relaxed);
  }
  attachNodeInit();
}

void CmiRegionPeInit(void) {
  CpvInitialize(PeState *, regionPeState);
  CpvAccess(regionPeState) = new PeState();
  CpvAccess(regionPeState)->rank = CmiMyRank();
  CpvInitialize(int, regionFireHandlerIdx);
  CpvAccess(regionFireHandlerIdx) = CmiRegisterHandler(fireHandler);
  CpvInitialize(int, regionDoneHandlerIdx);
  CpvAccess(regionDoneHandlerIdx) = CmiRegisterHandler(doneHandler);
  attachPeInit();
}

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
  c->state = ObjState::Live;
  c->destroyFlags = 0;
  c->done = nullptr;
  c->doneArg = nullptr;

  FireMsg *fire = nullptr;
  if (action.kind != CMI_ACTION_NONE) {
    fire = static_cast<FireMsg *>(CmiAlloc(sizeof(FireMsg)));
    fire->rank = ps->rank;
    fire->index = index;
    fire->generation = c->generation;
    CmiSetHandler(fire, CpvAccess(regionFireHandlerIdx));
    reinterpret_cast<CmiMessageHeader *>(fire)->messageSize = sizeof(FireMsg);
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
    slot = (uint32_t)ps->regions.size();
    ps->regions.push_back(nullptr);
    ps->regionGen.push_back(0);
  }
  auto *r = new CmiRegionRec();
  r->base = base;
  r->bytes = bytes;
  r->kind = kind;
  r->runtimeAllocated = false;
  r->ownerPe = CmiMyPe();
  r->slot = slot;
  r->generation = ps->regionGen[slot];
  r->state = ObjState::Live;
  r->destroyFlags = 0;
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
  }
  if (c != nullptr) {
    t.flags |= kTargetHasCounter;
    if (c->action.kind != CMI_ACTION_NONE) t.flags |= kTargetQdCounted;
    t.counterIndex = c->index;
    t.counterGen = c->generation;
    t.sigKind = 0;
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
// The data paths within a process (design 5.2 (a), 6)

namespace {

CmiAttachmentRec *checkHandle(CmiAttachment a, const char *call) {
  if (a == nullptr || a->magic != CMI_TARGET_MAGIC)
    CmiAbort("%s: not an attachment from CmiAttachTarget, or one already "
             "detached\n", call);
#if CMI_REGION_CHECKS
  if (a->rec->state.load(std::memory_order_acquire) ==
      (int)AttachState::DetachedByOwner)
    CmiAbort("%s: the owner, PE %d, destroyed this target's %s and detached "
             "this process. %s\n", call, a->target.ownerPe,
             (a->target.flags & kTargetHasRegion) ? "region" : "counter",
             kReadinessAdvice);
#endif
  return a;
}

// The target's counter, checked against the target's generation in every
// build: the check reads the line the contribution writes next.
CounterLine *liveLine(CmiAttachmentRec *a, const char *call) {
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
    uint32_t k = a->seq.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t armed = L->armedRounds.load(std::memory_order_acquire);
    if (k > armed)
      abortReadiness(call, L,
                     "this attachment's contribution %u arrived while the "
                     "owner had armed %u rounds, so it would count in place "
                     "of another origin's", k, armed);
  }
#endif
  return L;
}

void contributeLocal(CmiCounter local, const char *call) {
  if (local->action.kind != CMI_ACTION_NONE) qdCount(1, 0);
  contribute(local->line, 1, call);
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
  CounterLine *L = (tg.flags & kTargetHasCounter) ? liveLine(a, "CmiPut") : nullptr;
  if (tg.flags & kTargetQdCounted) qdCount(1, 0);
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
  memcpy(dst, (const char *)(uintptr_t)tg.pieceAddr + offset, bytes);
  if (local != nullptr) contributeLocal(local, "CmiGet");
  return CMI_REGION_OK;
}

int CmiSignal(CmiAttachment t) {
  CmiAttachmentRec *a = checkHandle(t, "CmiSignal");
  const CmiTarget &tg = a->target;
  if (!(tg.flags & kTargetHasCounter)) return CMI_REGION_ENOCOUNTER;
  CounterLine *L = liveLine(a, "CmiSignal");
  if (tg.flags & kTargetQdCounted) qdCount(1, 0);
  contribute(L, 1, "CmiSignal");
  return CMI_REGION_OK;
}
