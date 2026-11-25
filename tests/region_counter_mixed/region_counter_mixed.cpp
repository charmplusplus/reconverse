/***************************************************************
  Regions and counters with origins in every locality (conv-region.h):
  PE 0 owns the regions and counters, and the origins are PE 0 itself,
  the other PEs of its process and the PEs of the other processes. Run it
  with one process (no network) or several. The argument names the case;
  it prints "<case> PASS" or aborts.

    mixed     a piece per PE and one counter: every PE puts into its piece
              every round, at sizes from 1 byte to 64 KiB; every byte is
              checked and every round fires exactly once. Each origin's
              local counter must see every put complete
    threshold the same with puts of CmiRegionPutMessageMaxBytes() bytes and
              of one byte more, in turn: the first go in a message, the
              others as puts with signal
    signal    counter-only targets, CmiSignal from every PE, N varying per
              round (1 to 7)
    get       each PE reads, at four sizes and offsets, the regions of the
              next PE and of the PE half the job away
    nocounter puts without a counter, each to its own place; the region's
              destroy completes only once every one has landed, so its
              done callback checks every byte
    reuse     the other processes put and signal without waiting and
              detach at once; the owner destroys the counter as soon as
              they say they are done, and the destroy must wait for every
              contribution to land: a new counter in the same index then
              sees no late one
  These must abort (the CTest entry matches the message):
    count_remote  a remote origin's second contribution before the re-arm
                  (error-checking builds)
    stale_remote  a signal from another process through a target whose
                  counter was destroyed
 ****************************************************************/

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "converse.h"
#include "conv-region.h"

static const char *g_mode = "";
static int g_rounds = 1000;
static const int kSizes[] = {1, 8, 64, 65, 256, 4096, 8200, 65536};
static const int kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);
static const int kPiece = 65536;

enum Op { OP_ATTACH, OP_DETACH, OP_SIGNAL, OP_ACK, OP_GO, OP_SETUP, OP_END,
          OP_POLL };

struct Ctl {
  char core[CmiMsgHeaderSizeBytes];
  int op, slot, round, ack, from, tag, arg;
  CmiTarget t, t2;
};

CpvDeclare(int, ctlIdx);

struct PeData {
  std::vector<CmiAttachment> atts;
  std::vector<unsigned char> buf, dst;
  CmiCounter local = nullptr;
  int completions = 0, issued = 0;
  CmiRegion region = nullptr;
  unsigned char *mem = nullptr;
};
CpvDeclare(PeData *, pd);

static void fail(const char *fmt, ...) {
  char s[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  CmiAbort("region_counter_mixed %s FAIL on PE %d: %s\n", g_mode, CmiMyPe(), s);
}

static void pass(const char *what) {
  CmiPrintf("region_counter_mixed %s PASS: %d processes, %d PEs; %s\n", g_mode,
            CmiNumNodes(), CmiNumPes(), what);
  CmiExit(0);
}

static unsigned char pat(int round, int origin, size_t i) {
  return (unsigned char)(round * 131 + origin * 31 + i * 7 + 1);
}

static void fill(unsigned char *p, int round, int origin, size_t from, size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = pat(round, origin, from + i);
}

static void expect(const unsigned char *p, int round, int origin, size_t from,
                   size_t n, const char *what) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != pat(round, origin, from + i))
      fail("%s: byte %zu is %d, expected %d (round %d, origin %d)", what,
           from + i, p[i], pat(round, origin, from + i), round, origin);
}

static int npes() { return CmiNumPes(); }

static Ctl *newCtl(int op) {
  Ctl *m = (Ctl *)CmiAlloc(sizeof(Ctl));
  memset((char *)m + CmiMsgHeaderSizeBytes, 0,
         sizeof(Ctl) - CmiMsgHeaderSizeBytes);
  m->op = op;
  m->from = CmiMyPe();
  CmiSetHandler(m, CpvAccess(ctlIdx));
  return m;
}

static void send(int pe, Ctl *m) { CmiSyncSendAndFree(pe, sizeof(Ctl), m); }

static void sendOp(int pe, int op, int slot, const CmiTarget *t, int ack,
                   int round = 0, int arg = 0) {
  Ctl *m = newCtl(op);
  m->slot = slot;
  if (t) m->t = *t;
  m->ack = ack;
  m->round = round;
  m->arg = arg;
  send(pe, m);
}

static void ack0(int tag, int round = 0, const CmiTarget *t = nullptr,
                 int slot = 0) {
  Ctl *m = newCtl(OP_ACK);
  m->tag = tag;
  m->round = round;
  m->slot = slot;
  if (t) m->t = *t;
  send(0, m);
}

static CmiAttachment &att(int slot) {
  auto &v = CpvAccess(pd)->atts;
  if ((int)v.size() <= slot) v.resize(slot + 1, nullptr);
  return v[slot];
}

static CmiAttachment attOf(Ctl *m) {
  CmiAttachment &a = att(m->slot);
  if (a == nullptr) a = CmiAttachTarget(&m->t);
  return a;
}

static unsigned char *buf(size_t n) {
  auto &b = CpvAccess(pd)->buf;
  if (b.size() < n) b.resize(n);
  return b.data();
}

static void (*g_onAck)(Ctl *) = nullptr;
static void (*g_onCtl)(Ctl *) = nullptr;

static void ctlHandler(void *vmsg) {
  Ctl *m = (Ctl *)vmsg;
  switch (m->op) {
  case OP_ATTACH:
    attOf(m);
    break;
  case OP_DETACH: {
    CmiAttachment &a = att(m->slot);
    if (a != nullptr) {
      CmiDetachTarget(a);
      a = nullptr;
    }
    break;
  }
  case OP_SIGNAL:
    if (CmiSignal(attOf(m)) != CMI_REGION_OK) fail("CmiSignal");
    break;
  case OP_ACK:
    g_onAck(m);
    break;
  default:
    g_onCtl(m);
    CmiFree(m);
    return;
  }
  if (m->ack && m->op != OP_ACK) ack0(m->op, m->round, nullptr, m->slot);
  CmiFree(m);
}

// ----------------------------------------------------------------- mixed
namespace mixed {
std::vector<unsigned char> mem;
CmiRegion r;
CmiCounter c;
std::vector<CmiTarget> t;
int round, acks, dones;
// threshold: puts of exactly CmiRegionPutMessageMaxBytes() bytes and of one
// byte more, in turn; each process computes the same sizes
bool thresholdMode = false;
int size(int rr) {
  if (!thresholdMode) return kSizes[rr % kNumSizes];
  size_t limit = CmiRegionPutMessageMaxBytes();
  if (limit + 1 > (size_t)kPiece) fail("the limit %zu is past a piece", limit);
  return (int)limit + (rr % 2 == 0 ? 1 : 0);
}
void sendRound(int rr) {
  for (int o = 0; o < npes(); o++) sendOp(o, OP_GO, o, &t[o], 0, rr);
}
void allDone(void *) {
  if (++dones < 2) return;
  if (thresholdMode)
    pass("puts of the message limit and one byte more, in every locality");
  else
    pass("same PE, same process and other processes in every round");
}
void onRound(void *, CmiCounter) {
  for (int o = 0; o < npes(); o++)
    expect(mem.data() + (size_t)o * kPiece, round, o, 0, size(round), "piece");
  if (round == g_rounds) {
    for (int o = 0; o < npes(); o++) sendOp(o, OP_END, o, nullptr, 0);
    return;
  }
  round++;
  CmiCounterRearm(c, npes());
  sendRound(round);
}
void onLocal(void *, CmiCounter) { CpvAccess(pd)->completions++; }
void onCtl(Ctl *m) {
  PeData *p = CpvAccess(pd);
  if (m->op == OP_GO) {
    if (p->local == nullptr)
      p->local = CmiCounterCreate(1, CmiCounterActionFn(onLocal, nullptr),
                                  CMI_COUNTER_AUTO_REARM);
    int n = size(m->round);
    unsigned char *b = buf(n);
    fill(b, m->round, m->slot, 0, n);
    int rc = CmiPut(attOf(m), 0, b, n, nullptr, p->local, nullptr);
    if (rc != CMI_REGION_OK) fail("CmiPut returned %d", rc);
    p->issued++;
  } else if (m->op == OP_END || m->op == OP_POLL) {
    // every source completion has run before the detach
    if (p->completions < p->issued) {
      sendOp(CmiMyPe(), OP_POLL, m->slot, nullptr, 0);
      return;
    }
    if (p->completions != p->issued)
      fail("%d source completions for %d puts", p->completions, p->issued);
    CmiDetachTarget(att(m->slot));
    att(m->slot) = nullptr;
    CmiCounterDestroy(p->local, 0, nullptr, nullptr);
    p->local = nullptr;
    ack0(OP_END);
  }
}
void onAck(Ctl *m) {
  if (m->tag == OP_END && ++acks == npes()) {
    CmiRegionDestroy(r, 0, allDone, nullptr);
    CmiCounterDestroy(c, 0, allDone, nullptr);
  }
}
void hooks() {
  g_onAck = onAck;
  g_onCtl = onCtl;
}
void thresholdHooks() {
  thresholdMode = true;
  hooks();
}
void start() {
  if (thresholdMode)
    CmiPrintf("region_counter_mixed threshold: puts of %zu and %zu bytes\n",
              CmiRegionPutMessageMaxBytes(), CmiRegionPutMessageMaxBytes() + 1);
  mem.assign((size_t)npes() * kPiece, 0);
  r = CmiRegionCreate(mem.data(), mem.size(), CMI_MEM_HOST, 0, 0);
  for (int o = 0; o < npes(); o++)
    CmiRegionAddPiece(r, (size_t)o * kPiece, kPiece, nullptr, 0);
  c = CmiCounterCreate(npes(), CmiCounterActionFn(onRound, nullptr), 0);
  for (int o = 0; o < npes(); o++) t.push_back(CmiTargetMake(r, o, c));
  round = 1;
  sendRound(1);
}
} // namespace mixed

// ---------------------------------------------------------------- signal
namespace sig {
CmiCounter c;
CmiTarget t;
int round, actions, acks;
int N(int r) { return 1 + r % 7; }
void sendRound(int r) {
  for (int i = 0; i < N(r); i++) sendOp((i * 3 + r) % npes(), OP_SIGNAL, 0, &t, 0);
}
void done(void *) {
  if (actions != g_rounds) fail("%d actions for %d rounds", actions, g_rounds);
  pass("each round fired once");
}
void onRound(void *, CmiCounter) {
  actions++;
  if (CmiCounterValue(c) != 0 || actions != round)
    fail("action %d in round %d, value %d", actions, round, CmiCounterValue(c));
  if (round == g_rounds) {
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_DETACH, 0, nullptr, 1);
    return;
  }
  round++;
  CmiCounterRearm(c, N(round));
  sendRound(round);
}
void onAck(Ctl *) {
  if (++acks == npes()) CmiCounterDestroy(c, 0, done, nullptr);
}
void hooks() { g_onAck = onAck; }
void start() {
  c = CmiCounterCreate(N(1), CmiCounterActionFn(onRound, nullptr), 0);
  t = CmiTargetMake(nullptr, CMI_PIECE_ALL, c);
  round = 1;
  sendRound(1);
}
} // namespace sig

// ------------------------------------------------------------------- get
namespace getm {
const int kGets = 4;
const int kGetSize[kGets] = {1, 64, 4096, 65536 - 128};
const int kGetOffset[kGets] = {0, 7, 100, 128};
int got, ended;
int other(int which) {
  return which == 0 ? (CmiMyPe() + 1) % npes()
                    : (CmiMyPe() + npes() / 2) % npes();
}
void onGot(void *, CmiCounter) {
  PeData *p = CpvAccess(pd);
  size_t off = 0;
  for (int w = 0; w < 2; w++)
    for (int g = 0; g < kGets; g++) {
      expect(p->dst.data() + off, 0, other(w), kGetOffset[g], kGetSize[g], "get");
      off += kGetSize[g];
    }
  ack0(OP_GO);
}
void ownerDone(void *) { ack0(OP_END); }
void onCtl(Ctl *m) {
  PeData *p = CpvAccess(pd);
  if (m->op == OP_SETUP) {
    p->mem = (unsigned char *)CmiRegionAlloc(kPiece, CMI_MEM_HOST, 0, 0, &p->region);
    fill(p->mem, 0, CmiMyPe(), 0, kPiece);
    CmiTarget t = CmiTargetMake(p->region, CMI_PIECE_ALL, nullptr);
    // to the PEs that read it: the one before, and the one half the job away
    sendOp((CmiMyPe() + npes() - 1) % npes(), OP_GO, 0, &t, 0);
    sendOp((CmiMyPe() + npes() - npes() / 2) % npes(), OP_GO, 1, &t, 0);
  } else if (m->op == OP_GO) {
    attOf(m);
    if (att(0) == nullptr || att(1) == nullptr) return; // both targets first
    size_t total = 0;
    for (int g = 0; g < kGets; g++) total += kGetSize[g];
    p->dst.assign(2 * total, 0);
    p->local = CmiCounterCreate(2 * kGets, CmiCounterActionFn(onGot, nullptr), 0);
    size_t off = 0;
    for (int w = 0; w < 2; w++)
      for (int g = 0; g < kGets; g++) {
        if (CmiGet(p->dst.data() + off, kGetSize[g], nullptr, att(w),
                   kGetOffset[g], p->local, nullptr) != CMI_REGION_OK)
          fail("CmiGet");
        off += kGetSize[g];
      }
  } else if (m->op == OP_END) {
    CmiDetachTarget(att(0));
    CmiDetachTarget(att(1));
    att(0) = att(1) = nullptr;
    CmiCounterDestroy(p->local, 0, nullptr, nullptr);
    CmiRegionDestroy(p->region, 0, ownerDone, nullptr);
  }
}
void onAck(Ctl *m) {
  if (m->tag == OP_GO && ++got == npes()) {
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_END, 0, nullptr, 0);
  } else if (m->tag == OP_END && ++ended == npes()) {
    pass("gets from the next PE's region and from one half the job away");
  }
}
void hooks() {
  g_onAck = onAck;
  g_onCtl = onCtl;
}
void start() {
  for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_SETUP, 0, nullptr, 0);
}
} // namespace getm

// ------------------------------------------------------------- nocounter
namespace nocount {
const int kPuts = 64, kBytes = 512;
std::vector<unsigned char> mem;
CmiRegion r;
int acks;
void regionDone(void *) {
  for (int o = 0; o < npes(); o++)
    for (int k = 0; k < kPuts; k++)
      expect(mem.data() + ((size_t)o * kPuts + k) * kBytes, k, o, 0, kBytes,
             "a put without a counter, after the destroy");
  pass("the destroy waited for every put without a counter");
}
void onCtl(Ctl *m) {
  if (m->op != OP_GO) return;
  CmiAttachment a = attOf(m);
  // the sources stay untouched until the end: a put with no local counter
  // may still be reading its source after CmiPut returns
  unsigned char *b = buf((size_t)kPuts * kBytes);
  for (int k = 0; k < kPuts; k++) {
    fill(b + (size_t)k * kBytes, k, m->slot, 0, kBytes);
    if (CmiPut(a, (size_t)k * kBytes, b + (size_t)k * kBytes, kBytes, nullptr,
               nullptr, nullptr) != CMI_REGION_OK)
      fail("CmiPut");
  }
  CmiDetachTarget(a);
  att(m->slot) = nullptr;
  ack0(OP_GO);
}
void onAck(Ctl *) {
  if (++acks == npes()) CmiRegionDestroy(r, 0, regionDone, nullptr);
}
void hooks() {
  g_onAck = onAck;
  g_onCtl = onCtl;
}
void start() {
  mem.assign((size_t)npes() * kPuts * kBytes, 0);
  r = CmiRegionCreate(mem.data(), mem.size(), CMI_MEM_HOST, 0, 0);
  for (int o = 0; o < npes(); o++) {
    CmiRegionAddPiece(r, (size_t)o * kPuts * kBytes, (size_t)kPuts * kBytes,
                      nullptr, 0);
    CmiTarget t = CmiTargetMake(r, o, nullptr);
    sendOp(o, OP_GO, o, &t, 0);
  }
}
} // namespace nocount

// ----------------------------------------------------------------- reuse
namespace reuse {
const int kEach = 200;
std::vector<unsigned char> mem;
CmiRegion r;
CmiCounter c, c2;
CmiTarget tc, tr, t2; // on PE 0
int acks, origins, dones;
void never(void *, CmiCounter) {
  fail("a contribution of the destroyed counter reached the counter that "
       "reused its index");
}
void finish(void *, double) {
  CmiCounterDestroy(c2, 0, nullptr, nullptr);
  pass("the destroy waited for every contribution in flight");
}
void bothDone() {
  if (++dones < 2) return;
  // the index again, with a new generation; a late contribution of the old
  // counter would decrement it
  c2 = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
  t2 = CmiTargetMake(nullptr, CMI_PIECE_ALL, c2);
  if (t2.counterIndex != tc.counterIndex || t2.counterGen != tc.counterGen + 1)
    fail("the new counter has index %u generation %u, not %u and %u",
         t2.counterIndex, t2.counterGen, tc.counterIndex, tc.counterGen + 1);
  CcdCallFnAfter(finish, nullptr, 50.0);
}
void regionDone(void *) { bothDone(); }
void counterDone(void *) { bothDone(); }
void onCtl(Ctl *m) {
  if (m->op != OP_GO) return;
  // m->t: a piece of the region; m->t2: the counter alone
  CmiAttachment ar = CmiAttachTarget(&m->t);
  CmiAttachment as = CmiAttachTarget(&m->t2);
  // one source, never changed: puts with no local counter read it later
  unsigned char *b = buf(kSizes[kNumSizes - 1]);
  fill(b, 0, CmiMyPe(), 0, kSizes[kNumSizes - 1]);
  for (int k = 0; k < kEach; k++) {
    if (CmiPut(ar, 0, b, kSizes[k % kNumSizes], nullptr, nullptr, nullptr) !=
        CMI_REGION_OK)
      fail("CmiPut");
    if (CmiSignal(as) != CMI_REGION_OK) fail("CmiSignal");
  }
  CmiDetachTarget(ar);
  CmiDetachTarget(as);
  ack0(OP_GO);
}
void onAck(Ctl *) {
  if (++acks < origins) return;
  CmiRegionDestroy(r, 0, regionDone, nullptr);
  CmiCounterDestroy(c, 0, counterDone, nullptr);
}
void hooks() {
  g_onAck = onAck;
  g_onCtl = onCtl;
}
void start() {
  // the PEs of the other processes, or every PE of a single process
  std::vector<int> pes;
  for (int pe = 0; pe < npes(); pe++)
    if (CmiNumNodes() == 1 || CmiNodeOf(pe) != CmiMyNode()) pes.push_back(pe);
  origins = (int)pes.size();
  mem.assign((size_t)npes() * kSizes[kNumSizes - 1], 0);
  r = CmiRegionCreate(mem.data(), mem.size(), CMI_MEM_HOST, 0, 0);
  c = CmiCounterCreate(1 << 30, CmiCounterActionFn(never, nullptr), 0);
  tc = CmiTargetMake(nullptr, CMI_PIECE_ALL, c);
  for (size_t i = 0; i < pes.size(); i++) {
    CmiRegionAddPiece(r, (size_t)pes[i] * kSizes[kNumSizes - 1],
                      kSizes[kNumSizes - 1], nullptr, 0);
    tr = CmiTargetMake(r, (int)i, c);
    Ctl *m = newCtl(OP_GO);
    m->t = tr;
    m->t2 = tc;
    send(pes[i], m);
  }
}
} // namespace reuse

// ---------------------------------------------------------------- aborts
namespace aborts {
CmiCounter c;
CmiTarget t;
unsigned char mem[64];
int remotePe() { return CmiNumNodes() > 1 ? CmiNodeFirst(1) : 0; }
void quiet(void *, CmiCounter) {}
void countOnCtl(Ctl *m) {
  if (m->op != OP_GO) return;
  CmiAttachment a = attOf(m);
  unsigned char b[64] = {0};
  CmiPut(a, 0, b, sizeof(b), nullptr, nullptr, nullptr);
  CmiPut(a, 0, b, sizeof(b), nullptr, nullptr, nullptr); // before the re-arm
}
void countHooks() { g_onCtl = countOnCtl; }
void countRemote() {
  CmiRegion r = CmiRegionCreate(mem, sizeof(mem), CMI_MEM_HOST, 0, 0);
  c = CmiCounterCreate(1, CmiCounterActionFn(quiet, nullptr), 0);
  t = CmiTargetMake(r, CMI_PIECE_ALL, c);
  sendOp(remotePe(), OP_GO, 0, &t, 0);
}
void staleOnCtl(Ctl *m) {
  if (m->op == OP_SETUP) {
    attOf(m);
    ack0(OP_SETUP);
  } else if (m->op == OP_GO) {
    CmiSignal(att(0)); // through the destroyed counter's target
  }
}
void staleDone(void *) {
  // the index again, so a stale signal would find a live counter there
  CmiCounter c2 = CmiCounterCreate(5, CmiCounterActionFn(quiet, nullptr), 0);
  (void)c2;
  sendOp(remotePe(), OP_GO, 0, nullptr, 0);
}
void staleAck(Ctl *) { CmiCounterDestroy(c, CMI_DESTROY_DETACH_ALL, staleDone, nullptr); }
void staleHooks() {
  g_onCtl = staleOnCtl;
  g_onAck = staleAck;
}
void staleRemote() {
  c = CmiCounterCreate(5, CmiCounterActionFn(quiet, nullptr), 0);
  t = CmiTargetMake(nullptr, CMI_PIECE_ALL, c);
  sendOp(remotePe(), OP_SETUP, 0, &t, 0);
}
} // namespace aborts

// ------------------------------------------------------------------ main
static void (*g_start)() = nullptr;

CmiStartFn mymain(int argc, char **argv) {
  (void)argc;
  (void)argv;
  CpvInitialize(int, ctlIdx);
  CpvAccess(ctlIdx) = CmiRegisterHandler(ctlHandler);
  CpvInitialize(PeData *, pd);
  CpvAccess(pd) = new PeData();
  CmiNodeBarrier();
  if (CmiMyPe() == 0) g_start();
  return 0;
}

// hooks runs in every process: the handlers a case's messages reach
struct Case {
  const char *name;
  void (*hooks)();
  void (*start)();
};

static const Case kCases[] = {
    {"mixed", mixed::hooks, mixed::start},
    {"threshold", mixed::thresholdHooks, mixed::start},
    {"signal", sig::hooks, sig::start},
    {"get", getm::hooks, getm::start},
    {"nocounter", nocount::hooks, nocount::start},
    {"reuse", reuse::hooks, reuse::start},
    {"count_remote", aborts::countHooks, aborts::countRemote},
    {"stale_remote", aborts::staleHooks, aborts::staleRemote},
};
static void (*g_hooks)() = nullptr;

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "rounds=", 7) == 0) g_rounds = atoi(argv[i] + 7);
    for (const Case &c : kCases)
      if (g_start == nullptr && strcmp(argv[i], c.name) == 0) {
        g_mode = c.name;
        g_start = c.start;
        g_hooks = c.hooks;
      }
  }
  if (g_start == nullptr) {
    fprintf(stderr, "region_counter_mixed: name a case:");
    for (const Case &c : kCases) fprintf(stderr, " %s", c.name);
    fprintf(stderr, "\n");
    return 1;
  }
  g_onCtl = [](Ctl *) {};
  g_onAck = [](Ctl *) {};
  g_hooks();
  // a run that hangs or aborts still shows what it printed
  setvbuf(stdout, nullptr, _IONBF, 0);
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
