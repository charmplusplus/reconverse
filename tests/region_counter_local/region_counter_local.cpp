/***************************************************************
  Regions, counters and targets within one process (conv-region.h).

  Every case runs in one process: with +pe 1 the origins share the owner's
  PE, with more PEs they are other PEs of its process. The argument names
  the case; it prints "<case> PASS" or aborts.

    signal     counter-only targets: CmiSignal from every PE, N varying per
               round (1 to 7), re-armed in the action; destroy waits for the
               detach notices
    put        a user region with a piece per origin; two puts per origin
               per round at two offsets; every byte checked every round
    rounds     the same over CmiRegionAlloc memory, with the number of
               origins varying per round and the re-arm in a later handler
    auto       CMI_COUNTER_AUTO_REARM; each origin is paced by a credit
               counter of its own that the owner signals once it has read
               the round (design 4.9, pattern 3)
    local      source completion: an origin's local counter fires once per
               K puts
    get        CmiGet from the next PE's region at two offsets; the target's
               counter is not touched
    nocounter  puts through a target with no counter
    combos     one counter in targets of two regions; one piece in two
               targets with two counters; two buffers used by parity
    actions    a handler action (CmiCounterFiredMsg) and no action (the
               owner polls CmiCounterValue)
    teardown   index and slot reuse in FIFO order with new generations;
               destroy waits for every detach; CMI_DESTROY_DETACH_ALL; an
               attach to a destroyed region; a counter's destroy waits for
               the region it was paired with
    batch      CmiAttachTargets/CmiDetachTargets over 26 targets of every PE
    errors     the return codes of CmiPut, CmiGet and CmiSignal
    qd         the QD hook's created and processed counts
    early      a whole round arrives before the re-arm, which fires at once;
               an error-checking build aborts on the early contribution
    early_local  the same in every build, on a local counter, whose early
               completions are no error: two and three rounds before a
               re-arm fire once each
    queued     a counter destroyed while its message is queued, its index
               reused at once (queued_early: re-queued by an automatic
               re-arm, production builds)
    race       early contributions from the other PEs racing the owner's
               re-arm: race (automatic), race_explicit, race_handler;
               production builds
    attach     the other PEs attach and detach the same targets in a loop
               while the owner destroys them (attach_all: with
               CMI_DESTROY_DETACH_ALL halfway)
    auto_handler  a handler action with CMI_COUNTER_AUTO_REARM
  These must abort (the CTest entry matches the library's message):
    rearm_early  a re-arm before the round's action started (every build)
    stale        a signal through a target whose counter was destroyed: the
                 generation check of every build
    wrong_pe     a re-arm on a PE that does not own the counter
    table_limit  more counters than +counter_table allows
    round_skip   a second contribution through one attachment in one round
                 (error-checking builds, CMI_COUNTER_CHECK_ROUNDS)
    detached     a signal through an attachment whose owner destroyed the
                 counter with CMI_DESTROY_DETACH_ALL
    late_attach  an attach to a destroyed region is told to detach, and a
                 put through it is refused
    detach_all_put  another PE keeps putting while the owner destroys the
                 region with CMI_DESTROY_DETACH_ALL
  With +randomized_msgq, teardown does not check that a destroy waits for
  origins it may not have heard of yet: that relies on the default
  scheduler serving the urgent queue first.
 ****************************************************************/

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "converse.h"
#include "conv-region.h"

static const char *g_mode = "";
static bool g_randomized = false; // +randomized_msgq
static const int kRounds = 300;
static const int kPiece = 256;

enum Op {
  OP_ATTACH, OP_DETACH, OP_SIGNAL, OP_PUT, OP_ACK, OP_STEP, OP_SETUP, OP_GO,
  OP_END
};

struct Ctl {
  char core[CmiMsgHeaderSizeBytes];
  int op;
  int slot;   // which of the origin's attachments
  int round;
  int offset; // OP_PUT: offset within the piece
  int count;  // OP_PUT: bytes
  int ack;    // nonzero: answer PE 0 with OP_ACK
  int from;
  int tag;    // OP_ACK: what is acknowledged
  CmiTarget t, t2;
};

CpvDeclare(int, ctlIdx);
CpvDeclare(int, firedIdx);
CpvDeclare(int, autoFiredIdx);

// Each PE's origin state.
struct PeData {
  std::vector<CmiAttachment> atts;
  std::vector<unsigned char> buf;
  CmiCounter local = nullptr; // auto: credit; local: source completion
  int round = 0;
  // get and batch: what this PE owns
  CmiRegion region = nullptr;
  CmiCounter counter = nullptr;
  unsigned char *mem = nullptr;
  std::vector<unsigned char> dst, dst2;
  int owned = 0, base = 0;
};
CpvDeclare(PeData *, pd);

static void fail(const char *fmt, ...) {
  char s[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  CmiAbort("region_counter_local %s FAIL: %s\n", g_mode, s);
}

static void pass(const char *what) {
  CmiPrintf("region_counter_local %s PASS: %d PEs; %s\n", g_mode, CmiNumPes(),
            what);
  CmiExit(0);
}

static unsigned char pat(int round, int origin, size_t i) {
  return (unsigned char)(round * 131 + origin * 31 + i * 7 + 1);
}

static void fill(unsigned char *p, int round, int origin, size_t from,
                 size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = pat(round, origin, from + i);
}

static void expect(const unsigned char *p, int round, int origin, size_t from,
                   size_t n, const char *what) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != pat(round, origin, from + i))
      fail("%s: byte %zu is %d, expected %d (round %d, origin %d)", what,
           from + i, p[i], pat(round, origin, from + i), round, origin);
}

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

static void ack0(int tag, int round = 0, const CmiTarget *t = nullptr,
                 int slot = 0) {
  Ctl *m = newCtl(OP_ACK);
  m->tag = tag;
  m->round = round;
  m->slot = slot;
  if (t) m->t = *t;
  send(0, m);
}

static void sendOp(int pe, int op, int slot, const CmiTarget *t, int ack,
                   int round = 0, int offset = 0, int count = 0) {
  Ctl *m = newCtl(op);
  m->slot = slot;
  if (t) m->t = *t;
  m->ack = ack;
  m->round = round;
  m->offset = offset;
  m->count = count;
  send(pe, m);
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

static std::atomic<int> g_detached{0}; // CmiDetachTarget calls by OP_DETACH

// What each case does with the messages its origins and PE 0 receive.
static void (*g_onAck)(Ctl *) = nullptr; // on PE 0
static void (*g_onCtl)(Ctl *) = nullptr; // other ops, on any PE

static void ctlHandler(void *vmsg) {
  Ctl *m = (Ctl *)vmsg;
  switch (m->op) {
  case OP_ATTACH:
    attOf(m);
    break;
  case OP_DETACH: {
    CmiAttachment &a = att(m->slot);
    if (a != nullptr) {
      g_detached++; // before the detach, which may complete a destroy
      CmiDetachTarget(a);
      a = nullptr;
    }
    break;
  }
  case OP_SIGNAL: {
    int rc = CmiSignal(attOf(m));
    if (rc != CMI_REGION_OK) fail("CmiSignal returned %d", rc);
    break;
  }
  case OP_PUT: {
    unsigned char *b = buf(m->count);
    fill(b, m->round, m->slot, m->offset, m->count);
    int rc = CmiPut(attOf(m), m->offset, b, m->count, nullptr, nullptr,
                    nullptr);
    if (rc != CMI_REGION_OK) fail("CmiPut returned %d", rc);
    break;
  }
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

static int npes() { return CmiNumPes(); }

// ---------------------------------------------------------------- signal
namespace sig {
CmiCounter c;
CmiTarget t;
int round, actions, acks;
int N(int r) { return 1 + r % 7; }
void sendRound(int r) {
  for (int i = 0; i < N(r); i++) sendOp(i % npes(), OP_SIGNAL, 0, &t, 0);
}
void done(void *) {
  if (actions != kRounds) fail("%d actions for %d rounds", actions, kRounds);
  if (g_detached.load() != npes())
    fail("the destroy completed after %d of %d detaches", g_detached.load(),
         npes());
  pass("each round fired once; the destroy completed after every detach");
}
void onRound(void *, CmiCounter cc) {
  actions++;
  if (cc != c || CmiCounterValue(c) != 0 || actions != round)
    fail("action %d in round %d, value %d", actions, round,
         CmiCounterValue(c));
  if (round == kRounds) {
    // destroy first: it completes only once every PE has detached
    CmiCounterDestroy(c, 0, done, nullptr);
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_DETACH, 0, nullptr, 0);
    return;
  }
  round++;
  CmiCounterRearm(c, N(round));
  sendRound(round);
}
void onAck(Ctl *) {}
void start() {
  g_onAck = onAck;
  c = CmiCounterCreate(N(1), CmiCounterActionFn(onRound, nullptr), 0);
  t = CmiTargetMake(nullptr, CMI_PIECE_ALL, c);
  round = 1;
  sendRound(1);
}
} // namespace sig

// ---------------------------------------------------------- put, rounds
namespace put {
bool variable;
std::vector<unsigned char> userMem;
unsigned char *base;
CmiRegion r;
CmiCounter c;
std::vector<CmiTarget> t;
int round, actions, acks;
int K(int rr) { return variable ? 1 + rr % npes() : npes(); }
void sendRound(int rr) {
  for (int o = 0; o < K(rr); o++)
    for (int h = 0; h < 2; h++)
      sendOp(o, OP_PUT, o, &t[o], 0, rr, h * kPiece / 2, kPiece / 2);
}
void rearmAndSend() {
  round++;
  CmiCounterRearm(c, 2 * K(round));
  sendRound(round);
}
int dones;
void allDone(void *) {
  if (++dones == 2)
    pass(variable ? "variable origins, re-armed in a later handler"
                  : "two puts per origin per round");
}
void onRound(void *, CmiCounter) {
  actions++;
  if (actions != round) fail("action %d in round %d", actions, round);
  for (int o = 0; o < K(round); o++)
    expect(base + o * kPiece, round, o, 0, kPiece, "piece");
  if (round == kRounds) {
    for (int o = 0; o < npes(); o++) sendOp(o, OP_DETACH, o, nullptr, 1);
  } else if (variable) {
    sendOp(0, OP_STEP, 0, nullptr, 0);
  } else {
    rearmAndSend();
  }
}
void onCtl(Ctl *m) {
  if (m->op == OP_STEP) rearmAndSend();
}
void onAck(Ctl *) {
  if (++acks < npes()) return;
  CmiRegionDestroy(r, 0, allDone, nullptr);
  CmiCounterDestroy(c, 0, allDone, nullptr);
}
void start(bool var) {
  variable = var;
  g_onAck = onAck;
  g_onCtl = onCtl;
  size_t bytes = (size_t)npes() * kPiece;
  if (variable) {
    base = (unsigned char *)CmiRegionAlloc(bytes, CMI_MEM_HOST, 0, 0, &r);
  } else {
    userMem.assign(bytes, 0);
    base = userMem.data();
    r = CmiRegionCreate(base, bytes, CMI_MEM_HOST, 0, 0);
  }
  for (int o = 0; o < npes(); o++)
    if (CmiRegionAddPiece(r, (size_t)o * kPiece, kPiece, nullptr, 0) != o)
      fail("piece numbers");
  c = CmiCounterCreate(2 * K(1), CmiCounterActionFn(onRound, nullptr), 0);
  for (int o = 0; o < npes(); o++) t.push_back(CmiTargetMake(r, o, c));
  round = 1;
  sendRound(1);
}
} // namespace put

// ----------------------------------------------------------------- auto
namespace autom {
// PE 0
std::vector<unsigned char> mem;
CmiRegion r;
CmiCounter c;
std::vector<CmiTarget> t;
std::vector<CmiAttachment> credits;
int known, actions, acks;
bool pending;
// origins
void putNext() {
  PeData *p = CpvAccess(pd);
  p->round++;
  int o = CmiMyPe();
  unsigned char *b = buf(kPiece);
  fill(b, p->round, o, 0, kPiece);
  // no source completion: the buffer is reused only after the owner's
  // credit, which follows its read
  int rc = CmiPut(att(o), 0, b, kPiece, nullptr, nullptr, nullptr);
  if (rc != CMI_REGION_OK) fail("CmiPut returned %d", rc);
}
void onCredit(void *, CmiCounter) { putNext(); }
void creditDone(void *) { ack0(OP_END); }
void signalCredits() {
  for (CmiAttachment a : credits)
    if (CmiSignal(a) != CMI_REGION_OK) fail("CmiSignal");
}
void counterDone(void *) { pass("automatic re-arm, paced by credits"); }
void onRound(void *, CmiCounter cc) {
  actions++;
  if (CmiCounterValue(cc) != npes())
    fail("the counter was not re-armed before the action (value %d)",
         CmiCounterValue(cc));
  for (int o = 0; o < npes(); o++)
    expect(mem.data() + o * kPiece, actions, o, 0, kPiece, "piece");
  if (actions == kRounds) {
    for (CmiAttachment a : credits) CmiDetachTarget(a);
    credits.clear();
    for (int o = 0; o < npes(); o++) sendOp(o, OP_END, o, nullptr, 0);
    return;
  }
  if (known < npes()) pending = true;
  else signalCredits();
}
void onCtl(Ctl *m) {
  PeData *p = CpvAccess(pd);
  int o = CmiMyPe();
  if (m->op == OP_SETUP) {
    att(o) = CmiAttachTarget(&m->t);
    p->local = CmiCounterCreate(
        1, CmiCounterActionFn(onCredit, nullptr), CMI_COUNTER_AUTO_REARM);
    CmiTarget credit = CmiTargetMake(nullptr, CMI_PIECE_ALL, p->local);
    ack0(OP_SETUP, 0, &credit, o);
    putNext();
  } else if (m->op == OP_END) {
    CmiDetachTarget(att(o));
    att(o) = nullptr;
    CmiCounterDestroy(p->local, 0, creditDone, nullptr);
    p->local = nullptr;
  }
}
void onAck(Ctl *m) {
  if (m->tag == OP_SETUP) {
    credits[m->slot] = CmiAttachTarget(&m->t);
    if (++known == npes() && pending) {
      pending = false;
      signalCredits();
    }
  } else if (m->tag == OP_END && ++acks == npes()) {
    CmiRegionDestroy(r, 0, nullptr, nullptr);
    CmiCounterDestroy(c, 0, counterDone, nullptr);
  }
}
void start() {
  g_onAck = onAck;
  g_onCtl = onCtl;
  mem.assign((size_t)npes() * kPiece, 0);
  r = CmiRegionCreate(mem.data(), mem.size(), CMI_MEM_HOST, 0, 0);
  for (int o = 0; o < npes(); o++)
    CmiRegionAddPiece(r, (size_t)o * kPiece, kPiece, nullptr, 0);
  c = CmiCounterCreate(npes(), CmiCounterActionFn(onRound, nullptr),
                       CMI_COUNTER_AUTO_REARM);
  credits.assign(npes(), nullptr);
  for (int o = 0; o < npes(); o++) {
    CmiTarget tt = CmiTargetMake(r, o, c);
    sendOp(o, OP_SETUP, o, &tt, 0);
  }
}
} // namespace autom

// ---------------------------------------------------------------- local
namespace localm {
const int K = 4;
std::vector<unsigned char> mem;
CmiRegion r;
CmiCounter c;
std::vector<CmiTarget> t;
int round, localAcks, endAcks;
bool fired;
void sendRound(int rr) {
  for (int o = 0; o < npes(); o++) sendOp(o, OP_GO, o, &t[o], 0, rr);
}
void maybeNext() {
  if (!fired || localAcks < npes()) return;
  fired = false;
  localAcks = 0;
  if (round == kRounds) {
    for (int o = 0; o < npes(); o++) sendOp(o, OP_END, o, nullptr, 0);
    return;
  }
  round++;
  CmiCounterRearm(c, K * npes());
  sendRound(round);
}
void onRound(void *, CmiCounter) {
  for (int o = 0; o < npes(); o++)
    expect(mem.data() + o * kPiece, round, o, 0, kPiece, "piece");
  fired = true;
  maybeNext();
}
// on an origin, once per K puts
void onLocal(void *, CmiCounter) { ack0(OP_GO, CpvAccess(pd)->round); }
void counterDone(void *) { pass("one source completion per K puts"); }
void onCtl(Ctl *m) {
  PeData *p = CpvAccess(pd);
  if (m->op == OP_GO) {
    if (p->local == nullptr)
      p->local = CmiCounterCreate(K, CmiCounterActionFn(onLocal, nullptr),
                                  CMI_COUNTER_AUTO_REARM);
    p->round = m->round;
    CmiAttachment a = attOf(m);
    unsigned char *b = buf(kPiece);
    fill(b, m->round, m->slot, 0, kPiece);
    for (int j = 0; j < K; j++) {
      int rc = CmiPut(a, j * (kPiece / K), b + j * (kPiece / K), kPiece / K,
                      nullptr, p->local, nullptr);
      if (rc != CMI_REGION_OK) fail("CmiPut returned %d", rc);
    }
  } else if (m->op == OP_END) {
    CmiDetachTarget(att(m->slot));
    att(m->slot) = nullptr;
    CmiCounterDestroy(p->local, 0, nullptr, nullptr);
    p->local = nullptr;
    ack0(OP_END);
  }
}
void onAck(Ctl *m) {
  if (m->tag == OP_GO) {
    if (m->round != round) fail("source completion for round %d in round %d",
                                m->round, round);
    localAcks++;
    maybeNext();
  } else if (m->tag == OP_END && ++endAcks == npes()) {
    CmiRegionDestroy(r, 0, nullptr, nullptr);
    CmiCounterDestroy(c, 0, counterDone, nullptr);
  }
}
void start() {
  g_onAck = onAck;
  g_onCtl = onCtl;
  mem.assign((size_t)npes() * kPiece, 0);
  r = CmiRegionCreate(mem.data(), mem.size(), CMI_MEM_HOST, 0, 0);
  for (int o = 0; o < npes(); o++)
    CmiRegionAddPiece(r, (size_t)o * kPiece, kPiece, nullptr, 0);
  c = CmiCounterCreate(K * npes(), CmiCounterActionFn(onRound, nullptr), 0);
  for (int o = 0; o < npes(); o++) t.push_back(CmiTargetMake(r, o, c));
  round = 1;
  sendRound(1);
}
} // namespace localm

// ------------------------------------------------------------------ get
namespace getm {
const int kBytes = 128;
int got, ended;
void untouched(void *, CmiCounter) { fail("a get decremented the target's counter"); }
void onGot(void *, CmiCounter) {
  PeData *p = CpvAccess(pd);
  int owner = (CmiMyPe() + npes() - 1) % npes();
  expect(p->dst.data(), 0, owner, 0, kBytes, "whole region");
  expect(p->dst2.data(), 0, owner, 32, 16, "16 bytes at offset 32");
  ack0(OP_GO);
}
void ownerDone(void *) { ack0(OP_END); }
void onCtl(Ctl *m) {
  PeData *p = CpvAccess(pd);
  if (m->op == OP_SETUP) {
    p->mem = (unsigned char *)CmiRegionAlloc(kBytes, CMI_MEM_HOST, 0, 0,
                                             &p->region);
    fill(p->mem, 0, CmiMyPe(), 0, kBytes);
    p->counter = CmiCounterCreate(1, CmiCounterActionFn(untouched, nullptr), 0);
    CmiTarget t = CmiTargetMake(p->region, CMI_PIECE_ALL, p->counter);
    sendOp((CmiMyPe() + 1) % npes(), OP_GO, 0, &t, 0);
  } else if (m->op == OP_GO) {
    CmiAttachment a = attOf(m);
    p->local = CmiCounterCreate(2, CmiCounterActionFn(onGot, nullptr), 0);
    p->dst.assign(kBytes, 0);
    p->dst2.assign(16, 0);
    if (CmiGet(p->dst.data(), kBytes, nullptr, a, 0, p->local, nullptr) !=
            CMI_REGION_OK ||
        CmiGet(p->dst2.data(), 16, nullptr, a, 32, p->local, nullptr) !=
            CMI_REGION_OK)
      fail("CmiGet");
  } else if (m->op == OP_END) {
    if (CmiCounterValue(p->counter) != 1)
      fail("the target's counter is %d, not 1", CmiCounterValue(p->counter));
    CmiDetachTarget(att(0));
    att(0) = nullptr;
    CmiCounterDestroy(p->local, 0, nullptr, nullptr);
    CmiRegionDestroy(p->region, 0, nullptr, nullptr);
    CmiCounterDestroy(p->counter, 0, ownerDone, nullptr);
  }
}
void onAck(Ctl *m) {
  if (m->tag == OP_GO && ++got == npes()) {
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_END, 0, nullptr, 0);
  } else if (m->tag == OP_END && ++ended == npes()) {
    pass("gets from the next PE's region; the target's counter untouched");
  }
}
void start() {
  g_onAck = onAck;
  g_onCtl = onCtl;
  for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_SETUP, 0, nullptr, 0);
}
} // namespace getm

// ------------------------------------------------------------ nocounter
namespace nocount {
const int kR = 20;
std::vector<unsigned char> mem;
CmiRegion r;
std::vector<CmiTarget> t;
int round, acks;
void sendRound(int rr) {
  for (int o = 0; o < npes(); o++)
    sendOp(o, OP_PUT, o, &t[o], 1, rr, 0, kPiece);
}
void regionDone(void *) { pass("puts through targets with no counter"); }
void onAck(Ctl *m) {
  if (m->tag == OP_PUT) {
    if (++acks < npes()) return;
    acks = 0;
    for (int o = 0; o < npes(); o++)
      expect(mem.data() + o * kPiece, round, o, 0, kPiece, "piece");
    if (round < kR) {
      sendRound(++round);
    } else {
      for (int o = 0; o < npes(); o++) sendOp(o, OP_DETACH, o, nullptr, 1);
    }
  } else if (m->tag == OP_DETACH && ++acks == npes()) {
    CmiRegionDestroy(r, 0, regionDone, nullptr);
  }
}
void start() {
  g_onAck = onAck;
  mem.assign((size_t)npes() * kPiece, 0);
  r = CmiRegionCreate(mem.data(), mem.size(), CMI_MEM_HOST, 0, 0);
  for (int o = 0; o < npes(); o++) {
    CmiRegionAddPiece(r, (size_t)o * kPiece, kPiece, nullptr, 0);
    t.push_back(CmiTargetMake(r, o, nullptr));
  }
  round = 1;
  sendRound(1);
}
} // namespace nocount

// --------------------------------------------------------------- combos
namespace combos {
const int kB = 64;
int origin;
unsigned char *A, *B, *C, *E[2];
CmiRegion rA, rB, rC, rE[2];
CmiCounter cAB, c1, c2, d[2];
CmiTarget tA, tB, t1, t2, tE[2];
bool fired1, fired2;
int parityRound, acks, dones;
const int kParityRounds = 8;
void allDone(void *) {
  if (++dones == 10) pass("shared counter, shared piece, two buffers by parity");
}
void teardown() {
  for (int s = 0; s < 6; s++) sendOp(origin, OP_DETACH, s, nullptr, 1);
}
void parityPut(int rr) {
  int k = rr % 2;
  sendOp(origin, OP_PUT, 4 + k, &tE[k], 0, rr, 0, kB);
}
void onE(void *arg, CmiCounter) {
  int k = (int)(intptr_t)arg;
  if (k != parityRound % 2) fail("parity counter %d fired in round %d", k, parityRound);
  expect(E[k], parityRound, 4 + k, 0, kB, "this round's buffer");
  if (parityRound > 1)
    expect(E[1 - k], parityRound - 1, 4 + (1 - k), 0, kB,
           "the other buffer, still last round's");
  CmiCounterRearm(d[k], 1);
  if (parityRound == kParityRounds) teardown();
  else parityPut(++parityRound);
}
void onC(void *arg, CmiCounter) {
  if (arg == (void *)1) {
    // the same piece again, through the second target
    fired1 = true;
    sendOp(origin, OP_PUT, 3, &t2, 0, 3, 0, 32);
    return;
  }
  if (!fired1) fail("the second target's counter fired first");
  fired2 = true;
  // the put through t2 wrote the piece last; the bytes outside it are
  // untouched
  expect(C + 16, 3, 3, 0, 32, "the shared piece");
  for (int i = 0; i < 16; i++)
    if (C[i] != 0 || C[48 + i] != 0) fail("a byte outside the piece changed");
  parityRound = 1;
  parityPut(1);
}
void onAB(void *, CmiCounter) {
  expect(A, 1, 0, 0, kB, "region A");
  expect(B, 1, 1, 0, kB, "region B");
  // the same piece through two targets with two counters
  sendOp(origin, OP_PUT, 2, &t1, 0, 2, 0, 32);
}
void onAck(Ctl *m) {
  if (m->tag == OP_DETACH && ++acks == 6) {
    CmiRegion rs[5] = {rA, rB, rC, rE[0], rE[1]};
    CmiCounter cs[5] = {cAB, c1, c2, d[0], d[1]};
    for (CmiRegion x : rs) CmiRegionDestroy(x, 0, allDone, nullptr);
    for (CmiCounter x : cs) CmiCounterDestroy(x, 0, allDone, nullptr);
  }
}
void start() {
  g_onAck = onAck;
  origin = npes() - 1;
  A = (unsigned char *)CmiRegionAlloc(kB, CMI_MEM_HOST, 0, 0, &rA);
  B = (unsigned char *)CmiRegionAlloc(kB, CMI_MEM_HOST, 0, 0, &rB);
  C = (unsigned char *)CmiRegionAlloc(kB, CMI_MEM_HOST, 0, 0, &rC);
  memset(C, 0, kB);
  for (int k = 0; k < 2; k++)
    E[k] = (unsigned char *)CmiRegionAlloc(kB, CMI_MEM_HOST, 0, 0, &rE[k]);
  cAB = CmiCounterCreate(4, CmiCounterActionFn(onAB, nullptr), 0);
  tA = CmiTargetMake(rA, CMI_PIECE_ALL, cAB);
  tB = CmiTargetMake(rB, CMI_PIECE_ALL, cAB);
  CmiRegionAddPiece(rC, 16, 32, nullptr, 0);
  c1 = CmiCounterCreate(1, CmiCounterActionFn(onC, (void *)1), 0);
  c2 = CmiCounterCreate(1, CmiCounterActionFn(onC, (void *)2), 0);
  t1 = CmiTargetMake(rC, 0, c1);
  t2 = CmiTargetMake(rC, 0, c2);
  for (int k = 0; k < 2; k++) {
    d[k] = CmiCounterCreate(1, CmiCounterActionFn(onE, (void *)(intptr_t)k), 0);
    tE[k] = CmiTargetMake(rE[k], CMI_PIECE_ALL, d[k]);
  }
  // one counter for two regions: two puts into each, at two offsets
  for (int h = 0; h < 2; h++) {
    sendOp(origin, OP_PUT, 0, &tA, 0, 1, h * kB / 2, kB / 2);
    sendOp(origin, OP_PUT, 1, &tB, 0, 1, h * kB / 2, kB / 2);
  }
}
} // namespace combos

// -------------------------------------------------------------- actions
namespace actions {
const int kR = 5;
int origin;
CmiCounter h, z;
CmiTarget th, tz;
int hRound, zRound, acks, dones;
void allDone(void *) {
  if (++dones == 2) pass("a handler action and no action");
}
void startNone() {
  z = CmiCounterCreate(2, CmiCounterActionNone(), 0);
  tz = CmiTargetMake(nullptr, CMI_PIECE_ALL, z);
  zRound = 1;
  sendOp(origin, OP_SIGNAL, 1, &tz, 0);
  sendOp(origin, OP_SIGNAL, 1, &tz, 0);
  sendOp(0, OP_STEP, 0, nullptr, 0);
}
void firedHandler(void *vmsg) {
  CmiCounterFiredMsg *m = (CmiCounterFiredMsg *)vmsg;
  if (m->counter != h || (int)m->round != hRound)
    fail("handler action: counter %p round %u, expected %p round %d",
         (void *)m->counter, m->round, (void *)h, hRound);
  CmiFree(m);
  if (hRound == kR) {
    startNone();
    return;
  }
  hRound++;
  CmiCounterRearm(h, 1);
  sendOp(origin, OP_SIGNAL, 0, &th, 0);
}
void onCtl(Ctl *m) {
  if (m->op != OP_STEP) return;
  int v = CmiCounterValue(z);
  if (v < 0) fail("value %d", v);
  if (v > 0) {
    sendOp(0, OP_STEP, 0, nullptr, 0); // poll again
    return;
  }
  if (zRound == kR) {
    sendOp(origin, OP_DETACH, 0, nullptr, 1);
    sendOp(origin, OP_DETACH, 1, nullptr, 1);
    return;
  }
  zRound++;
  CmiCounterRearm(z, 2);
  sendOp(origin, OP_SIGNAL, 1, &tz, 0);
  sendOp(origin, OP_SIGNAL, 1, &tz, 0);
  sendOp(0, OP_STEP, 0, nullptr, 0);
}
void onAck(Ctl *) {
  if (++acks < 2) return;
  CmiCounterDestroy(h, 0, allDone, nullptr);
  CmiCounterDestroy(z, 0, allDone, nullptr);
}
void start() {
  g_onAck = onAck;
  g_onCtl = onCtl;
  origin = npes() - 1;
  h = CmiCounterCreate(1, CmiCounterActionHandler(CpvAccess(firedIdx)), 0);
  th = CmiTargetMake(nullptr, CMI_PIECE_ALL, h);
  hRound = 1;
  sendOp(origin, OP_SIGNAL, 0, &th, 0);
}
} // namespace actions

// ------------------------------------------------------------- teardown
namespace tdown {
int step, acks, last;
bool r1Done, k1Done, r2Done, k2Done, k2cDone, r4Called;
CmiRegion R1, R2, R3, R4;
CmiCounter K1, K2, K2c, K4;
CmiTarget T1, T2, T2c, T3;
std::vector<unsigned char> mem1(64), mem2(64);
void never(void *, CmiCounter) { fail("a counter with no contributions fired"); }
void next();
void r1(void *) {
  if (g_detached.load() != npes())
    fail("the region's destroy completed after %d of %d detaches",
         g_detached.load(), npes());
  r1Done = true;
  next();
}
void k1(void *) {
  k1Done = true;
  next();
}
// Step 2's destroys complete although no origin calls CmiDetachTarget
// before step 3 (the detached case checks the origins' handles).
void r2(void *) {
  r2Done = true;
  next();
}
void k2(void *) {
  k2Done = true;
  next();
}
void k2c(void *) {
  k2cDone = true;
  next();
}
void r3(void *) {
  // a registration for a destroyed generation is answered with a detach
  // request (late_attach checks that); the origin then detaches its handle
  sendOp(last, OP_ATTACH, 2, &T3, 1);
}
void k4(void *) {
  if (!r4Called) fail("the counter's destroy completed before its region's");
  pass("FIFO reuse, destroy completion, detach requests, late attach");
}
void fifo() {
  CmiCounter a = CmiCounterCreate(1, CmiCounterActionNone(), 0);
  CmiCounter b = CmiCounterCreate(1, CmiCounterActionNone(), 0);
  CmiCounter c = CmiCounterCreate(1, CmiCounterActionNone(), 0);
  CmiTarget ta = CmiTargetMake(nullptr, 0, a), tb = CmiTargetMake(nullptr, 0, b);
  CmiCounterDestroy(b, 0, nullptr, nullptr);
  CmiCounterDestroy(a, 0, nullptr, nullptr);
  CmiCounter x = CmiCounterCreate(1, CmiCounterActionNone(), 0);
  CmiCounter y = CmiCounterCreate(1, CmiCounterActionNone(), 0);
  CmiTarget tx = CmiTargetMake(nullptr, 0, x), ty = CmiTargetMake(nullptr, 0, y);
  if (tx.counterIndex != tb.counterIndex || tx.counterGen != tb.counterGen + 1 ||
      ty.counterIndex != ta.counterIndex || ty.counterGen != ta.counterGen + 1)
    fail("counter reuse: (%u,%u) (%u,%u) after freeing (%u,%u) then (%u,%u)",
         tx.counterIndex, tx.counterGen, ty.counterIndex, ty.counterGen,
         tb.counterIndex, tb.counterGen, ta.counterIndex, ta.counterGen);
  CmiCounterDestroy(x, 0, nullptr, nullptr);
  CmiCounterDestroy(y, 0, nullptr, nullptr);
  CmiCounterDestroy(c, 0, nullptr, nullptr);
  CmiRegion p = CmiRegionCreate(mem1.data(), 64, CMI_MEM_HOST, 0, 0);
  CmiRegion q = CmiRegionCreate(mem2.data(), 64, CMI_MEM_HOST, 0, 0);
  CmiTarget tp = CmiTargetMake(p, CMI_PIECE_ALL, nullptr);
  CmiTarget tq = CmiTargetMake(q, CMI_PIECE_ALL, nullptr);
  CmiRegionDestroy(q, 0, nullptr, nullptr);
  CmiRegionDestroy(p, 0, nullptr, nullptr);
  CmiRegion s = CmiRegionCreate(mem1.data(), 64, CMI_MEM_HOST, 0, 0);
  CmiRegion u = CmiRegionCreate(mem2.data(), 64, CMI_MEM_HOST, 0, 0);
  CmiTarget ts = CmiTargetMake(s, CMI_PIECE_ALL, nullptr);
  CmiTarget tu = CmiTargetMake(u, CMI_PIECE_ALL, nullptr);
  if (ts.regionSlot != tq.regionSlot || ts.regionGen != tq.regionGen + 1 ||
      tu.regionSlot != tp.regionSlot || tu.regionGen != tp.regionGen + 1)
    fail("region slot reuse");
  CmiRegionDestroy(s, 0, nullptr, nullptr);
  CmiRegionDestroy(u, 0, nullptr, nullptr);
}
void next() {
  if (step == 1 && r1Done && k1Done && acks == npes()) {
    // CMI_DESTROY_DETACH_ALL: the owner ends every registration itself, of
    // a region and of a counter-only target
    step = 2;
    acks = 0;
    CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &R2);
    K2 = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
    T2 = CmiTargetMake(R2, CMI_PIECE_ALL, K2);
    K2c = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
    T2c = CmiTargetMake(nullptr, CMI_PIECE_ALL, K2c);
    for (int pe = 0; pe < npes(); pe++) {
      sendOp(pe, OP_ATTACH, 1, &T2, 1);
      sendOp(pe, OP_ATTACH, 3, &T2c, 1);
    }
  } else if (step == 2 && r2Done && k2Done && k2cDone) {
    step = 3;
    acks = 0;
    for (int pe = 0; pe < npes(); pe++) {
      sendOp(pe, OP_DETACH, 1, nullptr, 1);
      sendOp(pe, OP_DETACH, 3, nullptr, 1);
    }
  }
}
// The last attach ack has arrived, and the registration was queued before
// it: one more pass of the default scheduler, which serves the urgent queue
// first, has handled it before this runs (not so with +randomized_msgq).
// Step 5 runs here after the counter's destroy was called, and only then
// destroys the region the counter's destroy waits for.
void onCtl(Ctl *m) {
  if (m->op != OP_STEP) return;
  if (step == 1) {
    CmiRegionDestroy(R1, 0, r1, nullptr);
    CmiCounterDestroy(K1, 0, k1, nullptr);
    if (!g_randomized && (r1Done || k1Done))
      fail("a destroy completed while origins were attached");
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_DETACH, 0, nullptr, 1);
  } else if (step == 2) {
    CmiRegionDestroy(R2, CMI_DESTROY_DETACH_ALL, r2, nullptr);
    CmiCounterDestroy(K2, 0, k2, nullptr);
    CmiCounterDestroy(K2c, CMI_DESTROY_DETACH_ALL, k2c, nullptr);
  } else if (step == 5) {
    r4Called = true;
    CmiRegionDestroy(R4, 0, nullptr, nullptr);
  }
}
void onAck(Ctl *m) {
  if (step == 1) {
    if (m->tag == OP_ATTACH && ++acks == npes()) {
      acks = 0;
      sendOp(0, OP_STEP, 0, nullptr, 0);
    } else if (m->tag == OP_DETACH) {
      acks++;
      next();
    }
  } else if (step == 2 && m->tag == OP_ATTACH && ++acks == 2 * npes()) {
    sendOp(0, OP_STEP, 0, nullptr, 0);
  } else if (step == 3 && m->tag == OP_DETACH && ++acks == 2 * npes()) {
    step = 4;
    CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &R3);
    T3 = CmiTargetMake(R3, CMI_PIECE_ALL, nullptr);
    CmiRegionDestroy(R3, 0, r3, nullptr);
  } else if (step == 4 && m->tag == OP_ATTACH) {
    sendOp(last, OP_DETACH, 2, nullptr, 1);
  } else if (step == 4 && m->tag == OP_DETACH) {
    step = 5;
    CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &R4);
    K4 = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
    CmiTargetMake(R4, CMI_PIECE_ALL, K4);
    CmiCounterDestroy(K4, 0, k4, nullptr);
    sendOp(0, OP_STEP, 0, nullptr, 0);
  }
}
void start() {
  g_onAck = onAck;
  g_onCtl = onCtl;
  last = npes() - 1;
  fifo();
  step = 1;
  CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &R1);
  K1 = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
  T1 = CmiTargetMake(R1, CMI_PIECE_ALL, K1);
  for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_ATTACH, 0, &T1, 1);
}
} // namespace tdown

// ---------------------------------------------------------------- batch
namespace batch {
const int kTotal = 26, kB = 16;
std::vector<CmiTarget> T(kTotal);
std::vector<CmiAttachment> A(kTotal);
int known, full, ended;
int ownedBy(int pe) { return kTotal / npes() + (pe < kTotal % npes() ? 1 : 0); }
int baseOf(int pe) {
  int b = 0;
  for (int q = 0; q < pe; q++) b += ownedBy(q);
  return b;
}
void onFull(void *, CmiCounter) {
  PeData *p = CpvAccess(pd);
  for (int j = 0; j < p->owned; j++)
    expect(p->mem + j * kB, 1, p->base + j, 0, kB, "batch piece");
  ack0(OP_GO);
}
void ownerDone(void *) { ack0(OP_END); }
void onCtl(Ctl *m) {
  PeData *p = CpvAccess(pd);
  if (m->op == OP_SETUP) {
    p->owned = ownedBy(CmiMyPe());
    p->base = baseOf(CmiMyPe());
    p->mem = (unsigned char *)CmiRegionAlloc((size_t)p->owned * kB, CMI_MEM_HOST,
                                             0, 0, &p->region);
    p->counter = CmiCounterCreate(p->owned, CmiCounterActionFn(onFull, nullptr), 0);
    for (int j = 0; j < p->owned; j++) {
      CmiRegionAddPiece(p->region, (size_t)j * kB, kB, nullptr, 0);
      CmiTarget t = CmiTargetMake(p->region, j, p->counter);
      ack0(OP_SETUP, 0, &t, p->base + j);
    }
  } else if (m->op == OP_END) {
    CmiRegionDestroy(p->region, 0, nullptr, nullptr);
    CmiCounterDestroy(p->counter, 0, ownerDone, nullptr);
  }
}
void onAck(Ctl *m) {
  if (m->tag == OP_SETUP) {
    T[m->slot] = m->t;
    if (++known < kTotal) return;
    if (CmiAttachTargets(T.data(), kTotal, A.data()) != kTotal)
      fail("CmiAttachTargets");
    unsigned char b[kB];
    for (int i = 0; i < kTotal; i++) {
      fill(b, 1, i, 0, kB);
      if (CmiPut(A[i], 0, b, kB, nullptr, nullptr, nullptr) != CMI_REGION_OK)
        fail("CmiPut");
    }
  } else if (m->tag == OP_GO && ++full == npes()) {
    CmiDetachTargets(A.data(), kTotal);
    for (int i = 0; i < kTotal; i++)
      if (A[i] != nullptr) fail("CmiDetachTargets left a handle");
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_END, 0, nullptr, 0);
  } else if (m->tag == OP_END && ++ended == npes()) {
    pass("26 targets attached and detached in batches");
  }
}
void start() {
  g_onAck = onAck;
  g_onCtl = onCtl;
  for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_SETUP, 0, nullptr, 0);
}
} // namespace batch

// --------------------------------------------------------------- errors
namespace errors {
unsigned char mem[64];
void never(void *, CmiCounter) { fail("the counter fired"); }
void done(void *) { pass("ECAPACITY, ENOREGION, ENOCOUNTER, EUNSUPPORTED"); }
void check(int got, int want, const char *what) {
  if (got != want) fail("%s returned %d, expected %d", what, got, want);
}
void start() {
  CmiRegion r = CmiRegionCreate(mem, sizeof(mem), CMI_MEM_HOST, 0, 0);
  CmiRegionAddPiece(r, 0, 32, nullptr, 0);
  CmiCounter c = CmiCounterCreate(100, CmiCounterActionFn(never, nullptr), 0);
  CmiTarget tp = CmiTargetMake(r, 0, c), tc = CmiTargetMake(nullptr, 0, c),
            tn = CmiTargetMake(r, 0, nullptr);
  CmiAttachment ap = CmiAttachTarget(&tp), ac = CmiAttachTarget(&tc),
                an = CmiAttachTarget(&tn);
  unsigned char b[64] = {0};
  const CmiLayout *layout = (const CmiLayout *)b;
  check(CmiPut(ap, 0, b, 33, nullptr, nullptr, nullptr), CMI_REGION_ECAPACITY, "a 33-byte put into 32");
  check(CmiPut(ap, 32, b, 1, nullptr, nullptr, nullptr), CMI_REGION_ECAPACITY, "a put past the end");
  check(CmiPut(ap, 33, b, 0, nullptr, nullptr, nullptr), CMI_REGION_ECAPACITY, "an offset past the end");
  check(CmiPut(ap, 16, b, 16, nullptr, nullptr, nullptr), CMI_REGION_OK, "a put of the last 16 bytes");
  check(CmiPut(ap, 0, b, 8, layout, nullptr, nullptr), CMI_REGION_EUNSUPPORTED, "a put with a layout");
  check(CmiPut(ap, 0, b, 8, nullptr, nullptr, (void *)b), CMI_REGION_EUNSUPPORTED, "a put with a stream");
  check(CmiPut(ac, 0, b, 8, nullptr, nullptr, nullptr), CMI_REGION_ENOREGION, "a put through a counter-only target");
  check(CmiGet(b, 8, nullptr, ac, 0, nullptr, nullptr), CMI_REGION_ENOREGION, "a get through a counter-only target");
  check(CmiGet(b, 33, nullptr, ap, 0, nullptr, nullptr), CMI_REGION_ECAPACITY, "a 33-byte get from 32");
  check(CmiGet(b, 8, layout, ap, 0, nullptr, nullptr), CMI_REGION_EUNSUPPORTED, "a get with a layout");
  check(CmiSignal(an), CMI_REGION_ENOCOUNTER, "a signal through a target with no counter");
  check(CmiSignal(ac), CMI_REGION_OK, "a signal");
  if (CmiCounterValue(c) != 98) fail("value %d, expected 98", CmiCounterValue(c));
  CmiDetachTarget(ap);
  CmiDetachTarget(ac);
  CmiDetachTarget(an);
  CmiRegionDestroy(r, 0, nullptr, nullptr);
  CmiCounterDestroy(c, 0, done, nullptr);
}
} // namespace errors

// ------------------------------------------------------------------- qd
namespace qd {
std::atomic<long> created{0}, processed{0};
void hook(int c, int p) {
  created += c;
  processed += p;
}
CmiCounter q, q2, L;
CmiRegion reg;
CmiTarget tq, tq2;
CmiAttachment a2;
int round, step, acks, dones;
bool q2Fired, lFired;
unsigned char mem[64];
void balanced(const char *when) {
  if (created.load() != processed.load())
    fail("%s: created %ld, processed %ld", when, created.load(), processed.load());
}
void sendRound(int n, int ack) {
  for (int i = 0; i < n; i++) sendOp(i % npes(), OP_SIGNAL, 0, &tq, ack);
}
void allDone(void *) {
  if (++dones < 4) return;
  balanced("after the destroys");
  pass("created and processed balance; a partly filled round holds them apart");
}
void qDone(void *arg) {
  balanced("after destroying a counter in a partly filled round");
  allDone(arg);
}
void localsFired() {
  if (!(q2Fired && lFired)) return;
  balanced("after a put with a local counter");
  // a partly filled round: two of three
  step = 3;
  CmiCounterRearm(q, 3);
  sendRound(2, 1);
}
void onQ2(void *, CmiCounter) { q2Fired = true; localsFired(); }
void onL(void *, CmiCounter) { lFired = true; localsFired(); }
void onQ(void *, CmiCounter) {
  round++;
  if (round < 2) {
    CmiCounterRearm(q, 3);
    sendRound(3, 0);
    return;
  }
  balanced("after two full rounds");
  if (created.load() != 6) fail("created %ld after two rounds of 3", created.load());
  // a put that names a local counter with an action counts twice
  reg = CmiRegionCreate(mem, sizeof(mem), CMI_MEM_HOST, 0, 0);
  q2 = CmiCounterCreate(1, CmiCounterActionFn(onQ2, nullptr), 0);
  L = CmiCounterCreate(1, CmiCounterActionFn(onL, nullptr), 0);
  tq2 = CmiTargetMake(reg, CMI_PIECE_ALL, q2);
  a2 = CmiAttachTarget(&tq2);
  unsigned char b[64] = {0};
  if (CmiPut(a2, 0, b, 64, nullptr, L, nullptr) != CMI_REGION_OK) fail("CmiPut");
}
void onAck(Ctl *m) {
  if (step == 3 && m->tag == OP_SIGNAL && ++acks == 2) {
    if (created.load() - processed.load() != 2)
      fail("a partly filled round of 2: created %ld, processed %ld",
           created.load(), processed.load());
    step = 4;
    acks = 0;
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_DETACH, 0, nullptr, 1);
  } else if (step == 4 && m->tag == OP_DETACH && ++acks == npes()) {
    CmiDetachTarget(a2);
    CmiCounterDestroy(q, 0, qDone, nullptr);
    CmiRegionDestroy(reg, 0, allDone, nullptr);
    CmiCounterDestroy(q2, 0, allDone, nullptr);
    CmiCounterDestroy(L, 0, allDone, nullptr);
  }
}
void start() {
  g_onAck = onAck;
  CmiRegionSetQdHook(hook);
  q = CmiCounterCreate(3, CmiCounterActionFn(onQ, nullptr), 0);
  tq = CmiTargetMake(nullptr, CMI_PIECE_ALL, q);
  sendRound(3, 0);
}
} // namespace qd

// ---------------------------------------------------------------- early
namespace early {
unsigned char mem[64];
CmiRegion reg;
CmiCounter c;
CmiAttachment a;
int round;
void put(int r) {
  unsigned char b[64];
  fill(b, r, 0, 0, sizeof(b));
  if (CmiPut(a, 0, b, sizeof(b), nullptr, nullptr, nullptr) != CMI_REGION_OK)
    fail("CmiPut");
}
void onRound(void *, CmiCounter) {
  round++;
  expect(mem, round, 0, 0, sizeof(mem), "the round's data");
  if (round == 4) {
    CmiDetachTarget(a);
    CmiRegionDestroy(reg, 0, nullptr, nullptr);
    CmiCounterDestroy(c, 0, nullptr, nullptr);
    pass("each early round fired at its re-arm");
    return;
  }
  // the next round's contribution before the re-arm (design 4.2)
  put(round + 1);
  if (CmiCounterValue(c) != -1) fail("value %d before the re-arm", CmiCounterValue(c));
  CmiCounterRearm(c, 1);
  if (CmiCounterValue(c) != 0) fail("value %d after the re-arm", CmiCounterValue(c));
}
void start() {
  reg = CmiRegionCreate(mem, sizeof(mem), CMI_MEM_HOST, 0, 0);
  c = CmiCounterCreate(1, CmiCounterActionFn(onRound, nullptr), 0);
  CmiTarget t = CmiTargetMake(reg, CMI_PIECE_ALL, c);
  a = CmiAttachTarget(&t);
  put(1);
}
} // namespace early


// ---------------------------------------------------------- early_local
// A local counter's early completions are no error, so its re-arm fires at
// once in every build: two puts before the re-arm, then three.
namespace earlyLocal {
unsigned char mem[64];
CmiRegion reg;
CmiAttachment a;
CmiCounter L;
int actions, expectActions;
void puts(int n) {
  unsigned char b[64] = {0};
  for (int i = 0; i < n; i++)
    if (CmiPut(a, 0, b, sizeof(b), nullptr, L, nullptr) != CMI_REGION_OK)
      fail("CmiPut");
}
void onLocal(void *, CmiCounter) {
  actions++;
  if (actions == 2) {
    // a re-arm below zero: two more rounds are complete already
    puts(3);
    if (CmiCounterValue(L) != -3) fail("value %d, not -3", CmiCounterValue(L));
  }
  if (actions == 5) {
    if (CmiCounterValue(L) != 0) fail("value %d after five rounds", CmiCounterValue(L));
    CmiDetachTarget(a);
    CmiCounterDestroy(L, 0, nullptr, nullptr);
    CmiRegionDestroy(reg, 0, nullptr, nullptr);
    pass("early local completions: each round fired once, at its re-arm");
    return;
  }
  CmiCounterRearm(L, 1);
}
void start() {
  reg = CmiRegionCreate(mem, sizeof(mem), CMI_MEM_HOST, 0, 0);
  CmiTarget t = CmiTargetMake(reg, CMI_PIECE_ALL, nullptr);
  a = CmiAttachTarget(&t);
  L = CmiCounterCreate(1, CmiCounterActionFn(onLocal, nullptr), 0);
  puts(2); // the second completes the next round before the re-arm
  if (CmiCounterValue(L) != -1) fail("value %d, not -1", CmiCounterValue(L));
}
} // namespace earlyLocal

// --------------------------------------------------------------- queued
// A counter destroyed while its message is queued, and its index reused at
// once: the message runs neither the old action nor the new one. With
// queued_early (production builds), an automatic counter whose next round
// arrives before its handler is destroyed in its action, with its message
// queued again.
namespace queued {
bool earlyRound;
int oldRuns, newRuns, autoRuns;
CmiCounter c2, ca;
CmiAttachment aa;
void oldAction(void *, CmiCounter) { oldRuns++; }
void finish(void *) {
  if (oldRuns != 0 || newRuns != 1 || autoRuns != 1)
    fail("old action %d times, new %d, automatic %d", oldRuns, newRuns, autoRuns);
  pass("a queued message of a destroyed counter runs no action");
}
void autoAction(void *, CmiCounter c) {
  autoRuns++;
  CmiDetachTarget(aa);
  CmiCounterDestroy(c, 0, finish, nullptr);
}
void newAction(void *, CmiCounter c) {
  newRuns++;
  ca = CmiCounterCreate(1, CmiCounterActionFn(autoAction, nullptr),
                        CMI_COUNTER_AUTO_REARM);
  CmiTarget t = CmiTargetMake(nullptr, 0, ca);
  aa = CmiAttachTarget(&t);
  CmiSignal(aa);                // round 1 fires
  if (earlyRound) CmiSignal(aa); // round 2, before the handler
  CmiCounterDestroy(c, 0, nullptr, nullptr);
}
void start() {
  CmiCounter c = CmiCounterCreate(1, CmiCounterActionFn(oldAction, nullptr), 0);
  CmiTarget t = CmiTargetMake(nullptr, 0, c);
  CmiAttachment a = CmiAttachTarget(&t);
  CmiSignal(a); // fires: the message is queued
  CmiDetachTarget(a);
  CmiCounterDestroy(c, 0, nullptr, nullptr);
  c2 = CmiCounterCreate(1, CmiCounterActionFn(newAction, nullptr), 0);
  CmiTarget t2 = CmiTargetMake(nullptr, 0, c2);
  CmiAttachment a2 = CmiAttachTarget(&t2);
  CmiSignal(a2);
  CmiDetachTarget(a2);
}
void startEarly() {
  earlyRound = true;
  start();
}
} // namespace queued

// ----------------------------------------------------------------- race
// Early contributions from PEs 1..P-1 racing the owner's re-arm (automatic
// or explicit) on PE 0; production builds. Each round fires exactly once;
// origins run at most one round ahead of the action.
namespace race {
const int kR = 20000;
bool autoMode = true, handlerMode = false;
std::atomic<long> total{0}, handled{0};
CmiCounter c;
int N;
void onRound(void *, CmiCounter cc) {
  long h = handled.load() + 1;
  int v = CmiCounterValue(cc);
  if (autoMode && v > N) fail("value %d above N after the automatic re-arm", v);
  if (v < -N) fail("round %ld: value %d below -N", h, v);
  if (!autoMode && h < kR) CmiCounterRearm(cc, N);
  handled.store(h, std::memory_order_release); // after the re-arm
  if (h == kR) pass("every round fired once");
  if (h > kR) fail("an extra round %ld", h);
}
void firedHandler(void *vm) {
  CmiCounterFiredMsg *m = (CmiCounterFiredMsg *)vm;
  long expect = handled.load() + 1;
  if ((long)m->round != expect) fail("round %u, expected %ld", m->round, expect);
  CmiCounter cc = m->counter;
  CmiFree(m);
  onRound(nullptr, cc);
}
void onCtl(Ctl *m) {
  if (m->op != OP_GO) return;
  CmiAttachment a = CmiAttachTarget(&m->t);
  for (long r = 1; r <= kR; r++) {
    while (total.load(std::memory_order_acquire) < (r - 1) * N ||
           handled.load(std::memory_order_acquire) < r - 2)
      std::this_thread::yield();
    if (CmiSignal(a) != CMI_REGION_OK) fail("CmiSignal");
    total.fetch_add(1, std::memory_order_acq_rel);
  }
  // the attachment stays: the program exits from PE 0
}
void start() {
  g_onCtl = onCtl;
  N = npes() - 1;
  if (N < 1) fail("needs two PEs or more");
  CmiCounterAction act = handlerMode ? CmiCounterActionHandler(CpvAccess(autoFiredIdx))
                                     : CmiCounterActionFn(onRound, nullptr);
  c = CmiCounterCreate(N, act, autoMode ? CMI_COUNTER_AUTO_REARM : 0);
  CmiTarget t = CmiTargetMake(nullptr, CMI_PIECE_ALL, c);
  for (int pe = 1; pe < npes(); pe++) sendOp(pe, OP_GO, 0, &t, 0);
}
void startExplicit() {
  autoMode = false;
  start();
}
void startHandler() {
  handlerMode = true;
  start();
}
} // namespace race

// --------------------------------------------------------------- attach
// PEs 1..P-1 attach and detach the same targets, one by one and in
// batches, while the owner destroys them: once they finish (attach), or
// halfway with CMI_DESTROY_DETACH_ALL (attach_all). Each destroy's done
// runs once.
namespace attachm {
const int kK = 3000;
bool detachAll = false;
std::atomic<long> iters{0};
std::atomic<int> finished{0};
int dones, polls;
bool destroyed;
CmiRegion R;
CmiCounter C;
void done(void *) { dones++; }
void poll() { sendOp(0, OP_STEP, 0, nullptr, 0); }
void onCtl(Ctl *m) {
  if (m->op == OP_STEP) {
    int origins = npes() - 1;
    long total = (long)kK * origins;
    if (!destroyed && ((detachAll && iters.load() >= total / 2) ||
                       finished.load() == origins)) {
      destroyed = true;
      int f = detachAll ? CMI_DESTROY_DETACH_ALL : 0;
      CmiRegionDestroy(R, f, done, nullptr);
      CmiCounterDestroy(C, f, done, nullptr);
    }
    if (finished.load() == origins && dones == 2 && ++polls > 1000)
      pass("concurrent attaches and detaches; each destroy completed once");
    else if (dones > 2)
      fail("%d destroy callbacks for two destroys", dones);
    else
      poll();
  } else if (m->op == OP_GO) {
    CmiTarget ts[3] = {m->t, m->t2, m->t};
    for (int i = 0; i < kK; i++) {
      CmiAttachment a = CmiAttachTarget(&m->t);
      CmiAttachment b = CmiAttachTarget(&m->t2);
      CmiAttachment v[3];
      CmiAttachTargets(ts, 3, v);
      if (i & 1) {
        CmiDetachTarget(a);
        CmiDetachTargets(v, 3);
      } else {
        CmiDetachTargets(v, 3);
        CmiDetachTarget(a);
      }
      CmiDetachTarget(b);
      iters.fetch_add(1);
      if ((i & 63) == 0) std::this_thread::yield();
    }
    finished.fetch_add(1);
  }
}
void start() {
  g_onCtl = onCtl;
  CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &R);
  C = CmiCounterCreate(1, CmiCounterActionNone(), 0);
  CmiTarget t = CmiTargetMake(R, CMI_PIECE_ALL, nullptr);
  CmiTarget tc = CmiTargetMake(nullptr, CMI_PIECE_ALL, C);
  for (int pe = 1; pe < npes(); pe++) {
    Ctl *m = newCtl(OP_GO);
    m->t = t;
    m->t2 = tc;
    send(pe, m);
  }
  poll();
}
void startAll() {
  detachAll = true;
  start();
}
} // namespace attachm

// --------------------------------------------------------- auto_handler
// A handler action with CMI_COUNTER_AUTO_REARM: the counter is re-armed
// before the handler gets its message.
namespace autoHandler {
const int kR = 200;
CmiCounter c;
CmiTarget t;
int round = 1;
void fired(void *vm) {
  CmiCounterFiredMsg *m = (CmiCounterFiredMsg *)vm;
  if (m->counter != c || (int)m->round != round)
    fail("round %u, expected %d", m->round, round);
  if (CmiCounterValue(c) != 1)
    fail("value %d: not re-armed before the handler", CmiCounterValue(c));
  CmiFree(m);
  if (round == kR) {
    CmiCounterDestroy(c, 0, nullptr, nullptr);
    for (int pe = 0; pe < npes(); pe++) sendOp(pe, OP_DETACH, 0, nullptr, 0);
    pass("a handler action re-armed automatically");
    return;
  }
  round++;
  sendOp(round % npes(), OP_SIGNAL, 0, &t, 0);
}
void start() {
  c = CmiCounterCreate(1, CmiCounterActionHandler(CpvAccess(autoFiredIdx)),
                       CMI_COUNTER_AUTO_REARM);
  t = CmiTargetMake(nullptr, CMI_PIECE_ALL, c);
  sendOp(round % npes(), OP_SIGNAL, 0, &t, 0);
}
} // namespace autoHandler

static void autoFiredHandler(void *vm) {
  if (strncmp(g_mode, "race", 4) == 0) race::firedHandler(vm);
  else autoHandler::fired(vm);
}

// --------------------------------------------------------------- aborts
namespace aborts {
CmiCounter c;
CmiAttachment a;
void never(void *, CmiCounter) {}
void rearmEarly() {
  c = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
  CmiTarget t = CmiTargetMake(nullptr, 0, c);
  a = CmiAttachTarget(&t);
  CmiSignal(a);             // the round fires; its action is queued
  CmiCounterRearm(c, 1);    // too early
  fail("the early re-arm was not refused");
}
void roundSkip() {
  c = CmiCounterCreate(2, CmiCounterActionFn(never, nullptr),
                       CMI_COUNTER_CHECK_ROUNDS);
  CmiTarget t = CmiTargetMake(nullptr, 0, c);
  a = CmiAttachTarget(&t);
  CmiSignal(a);
  CmiSignal(a); // this attachment's second contribution in round 1
  fail("the second contribution in one round was not refused");
}
// The owner has not yet handled the registration, so the destroy completes
// at once and the attachment is not yet detached: the generation check that
// every build has refuses the signal.
void stale() {
  c = CmiCounterCreate(5, CmiCounterActionFn(never, nullptr), 0);
  CmiTarget t = CmiTargetMake(nullptr, 0, c);
  a = CmiAttachTarget(&t);
  CmiCounterDestroy(c, 0, nullptr, nullptr);
  CmiSignal(a);
  fail("a signal through a destroyed counter's target was not refused");
}
// With CMI_DESTROY_DETACH_ALL the owner's detach request reaches this
// process before the done callback (the urgent queue first), and every
// build refuses the attachment itself.
void detachedDone(void *) {
  CmiSignal(a);
  fail("a signal through an attachment its owner ended was not refused");
}
void detached() {
  c = CmiCounterCreate(5, CmiCounterActionFn(never, nullptr), 0);
  CmiTarget t = CmiTargetMake(nullptr, 0, c);
  a = CmiAttachTarget(&t);
  CmiCounterDestroy(c, CMI_DESTROY_DETACH_ALL, detachedDone, nullptr);
}
// An attach to a destroyed region: the owner answers the registration with
// a detach request, after which every build refuses the put.
CmiTarget lateTarget;
void lateOnCtl(Ctl *m) {
  if (m->op != OP_STEP) return;
  unsigned char b[8] = {0};
  CmiPut(a, 0, b, sizeof(b), nullptr, nullptr, nullptr);
  fail("a put through an attachment to a destroyed region was not refused");
}
void lateAttach() {
  g_onCtl = lateOnCtl;
  CmiRegion r;
  CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &r);
  lateTarget = CmiTargetMake(r, CMI_PIECE_ALL, nullptr);
  CmiRegionDestroy(r, 0, nullptr, nullptr);
  a = CmiAttachTarget(&lateTarget);
  // the registration and the detach request are urgent: served first
  sendOp(0, OP_STEP, 0, nullptr, 0);
}
// PE 1 puts without pause into a region of PE 0, which destroys it with
// CMI_DESTROY_DETACH_ALL once the first put has arrived: once the detach
// request has run, PE 1's next put must abort, not write into the memory
// the destroy freed.
CmiRegion daRegion;
void daFailLater(void *, double) {
  fail("the origin kept putting after the owner ended its attachment");
}
void daDone(void *) { CcdCallFnAfter(daFailLater, nullptr, 2000.0); }
void daOnCtl(Ctl *m) {
  if (m->op != OP_GO) return;
  CmiAttachment pa = CmiAttachTarget(&m->t);
  unsigned char b[64] = {0};
  ack0(OP_GO);
  for (;;) {
    int rc = CmiPut(pa, 0, b, sizeof(b), nullptr, nullptr, nullptr);
    if (rc != CMI_REGION_OK) fail("CmiPut returned %d", rc);
  }
}
void daOnAck(Ctl *m) {
  if (m->tag == OP_GO)
    CmiRegionDestroy(daRegion, CMI_DESTROY_DETACH_ALL, daDone, nullptr);
}
void detachAllPut() {
  g_onCtl = daOnCtl;
  g_onAck = daOnAck;
  if (npes() < 2) fail("needs two PEs or more");
  CmiRegionAlloc(64, CMI_MEM_HOST, 0, 0, &daRegion);
  CmiTarget t = CmiTargetMake(daRegion, CMI_PIECE_ALL, nullptr);
  sendOp(1, OP_GO, 0, &t, 0);
}
void wrongOnCtl(Ctl *m) {
  if (m->op != OP_GO) return;
  CmiCounterRearm(c, 1);
  fail("a re-arm on PE %d was not refused", CmiMyPe());
}
void wrongPe() {
  g_onCtl = wrongOnCtl;
  if (npes() < 2) fail("needs two PEs or more");
  c = CmiCounterCreate(1, CmiCounterActionFn(never, nullptr), 0);
  sendOp(1, OP_GO, 0, nullptr, 0);
}
void tableLimit() {
  for (int i = 0; i < 4; i++)
    CmiCounterCreate(1, CmiCounterActionNone(), 0);
  fail("a fourth counter under +counter_table 3 was not refused");
}
} // namespace aborts

// ------------------------------------------------------------------ main
static void (*g_start)() = nullptr;

CmiStartFn mymain(int argc, char **argv) {
  (void)argc;
  // Bind this PE per +setcpuaffinity/+pemap (every PE takes part);
  // ConverseInit does not.
  CmiInitCPUAffinity(argv);
  CpvInitialize(int, ctlIdx);
  CpvAccess(ctlIdx) = CmiRegisterHandler(ctlHandler);
  CpvInitialize(int, firedIdx);
  CpvAccess(firedIdx) = CmiRegisterHandler(actions::firedHandler);
  CpvInitialize(int, autoFiredIdx);
  CpvAccess(autoFiredIdx) = CmiRegisterHandler(autoFiredHandler);
  CpvInitialize(PeData *, pd);
  CpvAccess(pd) = new PeData();
  CmiNodeBarrier();
  if (CmiMyPe() == 0) g_start();
  return 0;
}

struct Case {
  const char *name;
  void (*start)();
};

static void startPut() { put::start(false); }
static void startRounds() { put::start(true); }

static const Case kCases[] = {
    {"signal", sig::start},         {"put", startPut},
    {"rounds", startRounds},        {"auto", autom::start},
    {"local", localm::start},       {"get", getm::start},
    {"nocounter", nocount::start},  {"combos", combos::start},
    {"actions", actions::start},    {"teardown", tdown::start},
    {"batch", batch::start},        {"errors", errors::start},
    {"qd", qd::start},              {"early", early::start},
    {"early_local", earlyLocal::start},
    {"queued", queued::start},      {"queued_early", queued::startEarly},
    {"race", race::start},          {"race_explicit", race::startExplicit},
    {"race_handler", race::startHandler},
    {"attach", attachm::start},     {"attach_all", attachm::startAll},
    {"auto_handler", autoHandler::start},
    {"rearm_early", aborts::rearmEarly},
    {"round_skip", aborts::roundSkip},
    {"stale", aborts::stale},       {"detached", aborts::detached},
    {"late_attach", aborts::lateAttach},
    {"detach_all_put", aborts::detachAllPut},
    {"wrong_pe", aborts::wrongPe},  {"table_limit", aborts::tableLimit},
};

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "+randomized_msgq") == 0) g_randomized = true;
  for (int i = 1; i < argc && g_start == nullptr; i++)
    for (const Case &c : kCases)
      if (strcmp(argv[i], c.name) == 0) {
        g_mode = c.name;
        g_start = c.start;
      }
  if (g_start == nullptr) {
    fprintf(stderr, "region_counter_local: name a case:");
    for (const Case &c : kCases) fprintf(stderr, " %s", c.name);
    fprintf(stderr, "\n");
    return 1;
  }
  g_onCtl = [](Ctl *) {};
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
