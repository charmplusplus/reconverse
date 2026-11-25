// Put with signal (CmiIssueRputSignal): many concurrent signalled puts, each
// checked when its signal arrives.
//
// Every PE is a target with a window of W slots for each of its peers, and an
// origin that streams puts into the W slots it has on each peer. The peers of
// a PE: with several processes, every PE of the other processes, so every
// put crosses the network (and with +lci_ndevices every pair of devices
// meets); in one process, every other PE, which takes the copy within a
// process.
//
// Put s (s = 1..M) of a pair goes to slot (s-1) % W, has size sizeOf(s), and
// its payload begins and ends with s; the bytes between follow a pattern of
// (origin, s). The signal is the slot's index among its PE's slots, so every
// PE of a process uses the same signal values, and odd and even puts use two
// kinds. The handler, on whatever thread it runs, checks that its kind,
// destPe and signal name a slot that is waiting for a put of that kind, and
// every byte of the payload. A signal that arrives before its data, or before
// part of it, finds the previous put's bytes or the cleared head and tail of
// the slot, and the test aborts. Then the target PE clears the slot and
// credits the origin, which reuses the slot once it also has the put's local
// completion.
//
// Every origin also writes a 64-byte marker into each peer with
// CmiRdmaPutRaw, which the peer checks at the end.
//
// Usage: <launcher> -n P ./put_signal +pe N [puts per pair, default 2000]
//        [window, default 4]
#include "conv-rdma.h"
#include <converse.h>
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sizes cycled through: below and above the inject sizes of the providers
// (tcp, cxi), around LCI's buffer-copy limit (8 KiB packets), and large.
static const size_t kSizes[] = {16, 24, 64, 200, 1000, 4096, 8000, 8300, 65536};
static const int kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);
static const size_t kSlotBytes = 65536;
static const int kMaxWindow = 16;
static const size_t kRawBytes = 64;
static const int kMaxRanks = 1024;
static const uint64_t kCleared = ~(uint64_t)0;

static int putsPerPair = 2000;
static int window = 4;
static int kinds[2];
static int kindCtx[2] = {0, 1};

static size_t sizeOf(uint64_t s) { return kSizes[s % kNumSizes]; }
static int kindOf(uint64_t s) { return (int)(s & 1); }
static unsigned char pattern(int origin, uint64_t s, size_t i) {
  return (unsigned char)((origin * 131 + s * 7 + i) & 0xff);
}
static unsigned char rawPattern(int origin, size_t i) {
  return (unsigned char)((origin * 17 + i * 3 + 1) & 0xff);
}

// The peers of a PE, in PE order: the PEs of the other processes, or, with
// one process, the other PEs.
static int numPeers() {
  return CmiNumNodes() > 1 ? CmiNumPes() - CmiMyNodeSize() : CmiNumPes() - 1;
}
static int peerPe(int me, int i) {
  if (CmiNumNodes() > 1) {
    int first = CmiNodeFirst(CmiNodeOf(me));
    return i < first ? i : i + CmiMyNodeSize();
  }
  return i < me ? i : i + 1;
}
static int peerIndex(int me, int pe) {
  if (CmiNumNodes() > 1) {
    int first = CmiNodeFirst(CmiNodeOf(me));
    return pe < first ? pe : pe - CmiMyNodeSize();
  }
  return pe < me ? pe : pe - 1;
}

// Target side, read by the signal handler on any thread of the process.
struct Slot {
  char *buf;
  CmiNcpyBuffer reg;
  int origin;
  std::atomic<uint64_t> expect;  // the put this slot waits for; 0: none
};
struct TargetState {
  int pe;
  int nslots;
  Slot *slots;
  char *raw;
  CmiNcpyBuffer rawReg;
  long arrivals;
  long arrivalsByKind[2];
  double doneSince;
};
static TargetState *targets[kMaxRanks];

// Origin side, used by its own PE only.
struct OriginSlot {
  char *src;
  CmiNcpyBuffer srcReg;
  CmiNcpyBuffer dest;
  int target;
  uint16_t signal;
  uint64_t seq;
  bool localDone, credited;
};
struct OriginState {
  OriginSlot *slots;
  int slotsDone;
  char *raw;
  CmiNcpyBuffer rawReg;
};
CpvStaticDeclare(OriginState *, origin);
CpvStaticDeclare(int, doneMsgs);
CpvStaticDeclare(double, startTime);

struct Ack {
  int origin;
  int slot;
  uint64_t seq;
};

struct ExchangeMsg {
  char core[CmiMsgHeaderSizeBytes];
  int from;      // the target PE
  int slotBase;  // the signal of the origin's first slot there
  CmiNcpyBuffer dests[kMaxWindow];
  CmiNcpyBuffer raw;
};
struct AckMsg {
  char core[CmiMsgHeaderSizeBytes];
  Ack ack;
};
struct LandedMsg {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t seq;
  int kind;
  uint16_t signal;
};
struct CreditMsg {
  char core[CmiMsgHeaderSizeBytes];
  int from;
  int w;
  uint64_t seq;
};
struct DoneMsg {
  char core[CmiMsgHeaderSizeBytes];
};

static int exchangeIdx, ackIdx, landedIdx, creditIdx, doneIdx, recheckIdx;

static void fail(const char *what, int destPe, uint16_t signal, uint64_t s,
                 const char *buf, size_t sz) {
  uint64_t head, tail;
  memcpy(&head, buf, 8);
  memcpy(&tail, buf + sz - 8, 8);
  CmiAbort("put_signal: %s: PE %d signal %u, put %llu (%zu bytes): the slot "
           "holds head %llu, tail %llu\n",
           what, destPe, signal, (unsigned long long)s, sz,
           (unsigned long long)head, (unsigned long long)tail);
}

// Runs where the backend raised the signal: any thread of destPe's process.
static void signalHandler(void *ctx, int destPe, uint16_t signal, int srcNode,
                          size_t size) {
  int kind = *(int *)ctx;
  if (destPe < 0 || destPe >= CmiNumPes() || CmiNodeOf(destPe) != CmiMyNode())
    CmiAbort("put_signal: a signal for PE %d reached process %d\n", destPe,
             CmiMyNode());
  TargetState *t = targets[CmiRankOf(destPe)];
  if (signal >= t->nslots)
    CmiAbort("put_signal: signal %u is not a slot of PE %d (%d slots)\n",
             signal, destPe, t->nslots);
  Slot &sl = t->slots[signal];
  uint64_t s = sl.expect.load(std::memory_order_acquire);
  if (s == 0)
    CmiAbort("put_signal: PE %d slot %u got a signal after its last put\n",
             destPe, signal);
  if (kindOf(s) != kind)
    CmiAbort("put_signal: PE %d slot %u: put %llu is of kind %d, but its "
             "signal came to the handler of kind %d\n",
             destPe, signal, (unsigned long long)s, kinds[kindOf(s)],
             kinds[kind]);
  size_t sz = sizeOf(s);
  uint64_t head, tail;
  memcpy(&head, sl.buf, 8);
  memcpy(&tail, sl.buf + sz - 8, 8);
  if (head != s || tail != s)
    fail("the signal arrived before its data", destPe, signal, s, sl.buf, sz);
  for (size_t i = 8; i < sz - 8; i++)
    if ((unsigned char)sl.buf[i] != pattern(sl.origin, s, i))
      fail("the signal arrived before all of its data", destPe, signal, s,
           sl.buf, sz);
  if (srcNode != -1 && srcNode != CmiNodeOf(sl.origin))
    CmiAbort("put_signal: PE %d slot %u: the signal names process %d, but "
             "PE %d of process %d put it\n",
             destPe, signal, srcNode, sl.origin, CmiNodeOf(sl.origin));
  if (size != 0 && size != sz)
    CmiAbort("put_signal: PE %d slot %u: the signal reports %zu bytes, the "
             "put wrote %zu\n", destPe, signal, size, sz);
  LandedMsg *m = (LandedMsg *)CmiAlloc(sizeof(LandedMsg));
  CmiSetHandler(m, landedIdx);
  m->seq = s;
  m->kind = kind;
  m->signal = signal;
  CmiPushPE(CmiRankOf(destPe), m);
}

static void sendDone() {
  DoneMsg *m = (DoneMsg *)CmiAlloc(sizeof(DoneMsg));
  CmiSetHandler(m, doneIdx);
  CmiSyncSendAndFree(0, sizeof(DoneMsg), m);
}

// The markers CmiRdmaPutRaw wrote; nothing reports their completion, so
// look again for a while.
static bool rawLanded(TargetState *t) {
  for (int i = 0; i < numPeers(); i++) {
    int o = peerPe(t->pe, i);
    for (size_t b = 0; b < kRawBytes; b++)
      if ((unsigned char)t->raw[i * kRawBytes + b] != rawPattern(o, b))
        return false;
  }
  return true;
}

static void checkRaw(TargetState *t) {
  if (rawLanded(t)) {
    sendDone();
    return;
  }
  if (CmiWallTimer() - t->doneSince > 10.0)
    CmiAbort("put_signal: PE %d: a CmiRdmaPutRaw marker has not landed 10 s "
             "after the last signalled put\n", t->pe);
  DoneMsg *m = (DoneMsg *)CmiAlloc(sizeof(DoneMsg));
  CmiSetHandler(m, recheckIdx);
  CmiSyncSendAndFree(CmiMyPe(), sizeof(DoneMsg), m);
}

static void recheckHandler(void *msg) {
  CmiFree(msg);
  checkRaw(targets[CmiMyRank()]);
}

// On the target PE: clear the slot, wait for its next put, credit the origin.
static void landedHandler(void *msg) {
  LandedMsg *m = (LandedMsg *)msg;
  TargetState *t = targets[CmiMyRank()];
  Slot &sl = t->slots[m->signal];
  size_t sz = sizeOf(m->seq);
  memcpy(sl.buf, &kCleared, 8);
  memcpy(sl.buf + sz - 8, &kCleared, 8);
  uint64_t next = m->seq + window;
  sl.expect.store(next <= (uint64_t)putsPerPair ? next : 0,
                  std::memory_order_release);
  t->arrivals++;
  t->arrivalsByKind[m->kind]++;
  CreditMsg *c = (CreditMsg *)CmiAlloc(sizeof(CreditMsg));
  CmiSetHandler(c, creditIdx);
  c->from = CmiMyPe();
  c->w = m->signal % window;
  c->seq = m->seq;
  CmiSyncSendAndFree(sl.origin, sizeof(CreditMsg), c);
  CmiFree(msg);
  if (t->arrivals == (long)numPeers() * putsPerPair) {
    if (t->arrivalsByKind[0] == 0 || t->arrivalsByKind[1] == 0)
      CmiAbort("put_signal: PE %d saw only one kind\n", CmiMyPe());
    t->doneSince = CmiWallTimer();
    checkRaw(t);
  }
}

static void issue(int slotIdx) {
  OriginSlot &o = CpvAccess(origin)->slots[slotIdx];
  uint64_t s = o.seq;
  size_t sz = sizeOf(s);
  memcpy(o.src, &s, 8);
  for (size_t i = 8; i < sz - 8; i++)
    o.src[i] = (char)pattern(CmiMyPe(), s, i);
  memcpy(o.src + sz - 8, &s, 8);
  CmiNcpyBuffer src = o.srcReg;
  src.cnt = sz;
  CmiNcpyBuffer dst = o.dest;
  dst.cnt = sz;
  Ack ack = {CmiMyPe(), slotIdx, s};
  NcpyOperationInfo *info = src.createNcpyOpInfo(
      src, dst, sizeof(Ack), (char *)&ack, (char *)&ack, -1, CMK_DIRECT_API,
      nullptr);
  o.localDone = o.credited = false;
  CmiIssueRputSignal(info, o.target, kinds[kindOf(s)], o.signal);
}

static void slotDone() {
  OriginState *os = CpvAccess(origin);
  if (++os->slotsDone == numPeers() * window)
    sendDone();
}

static void maybeNext(int slotIdx) {
  OriginSlot &o = CpvAccess(origin)->slots[slotIdx];
  if (!o.localDone || !o.credited)
    return;
  o.seq += window;
  if (o.seq <= (uint64_t)putsPerPair)
    issue(slotIdx);
  else
    slotDone();
}

// The Direct API acknowledgement: the put's local completion, on any thread.
static void ncpyAckHandler(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;
  AckMsg *m = (AckMsg *)CmiAlloc(sizeof(AckMsg));
  CmiSetHandler(m, ackIdx);
  memcpy(&m->ack, info->srcAck, sizeof(Ack));
  CmiSyncSendAndFree(m->ack.origin, sizeof(AckMsg), m);
}

static void ackHandler(void *msg) {
  Ack ack = ((AckMsg *)msg)->ack;
  CmiFree(msg);
  OriginSlot &o = CpvAccess(origin)->slots[ack.slot];
  if (ack.seq != o.seq || o.localDone)
    CmiAbort("put_signal: PE %d: local completion of put %llu, expected %llu\n",
             CmiMyPe(), (unsigned long long)ack.seq,
             (unsigned long long)o.seq);
  o.localDone = true;
  maybeNext(ack.slot);
}

static void creditHandler(void *msg) {
  CreditMsg *c = (CreditMsg *)msg;
  int slotIdx = peerIndex(CmiMyPe(), c->from) * window + c->w;
  OriginSlot &o = CpvAccess(origin)->slots[slotIdx];
  if (c->seq != o.seq || o.credited)
    CmiAbort("put_signal: PE %d: credit for put %llu, expected %llu\n",
             CmiMyPe(), (unsigned long long)c->seq, (unsigned long long)o.seq);
  CmiFree(msg);
  o.credited = true;
  maybeNext(slotIdx);
}

// From a target: its slots for this origin. Start the puts.
static void exchangeHandler(void *msg) {
  ExchangeMsg *m = (ExchangeMsg *)msg;
  OriginState *os = CpvAccess(origin);
  int me = CmiMyPe();
  int idx = peerIndex(me, m->from);
  CmiNcpyBuffer rawSrc = os->rawReg;
  rawSrc.ptr = os->raw + (size_t)idx * kRawBytes;
  rawSrc.cnt = kRawBytes;
  CmiRdmaPutRaw(rawSrc, m->raw, (size_t)peerIndex(m->from, me) * kRawBytes);
  for (int w = 0; w < window; w++) {
    int slotIdx = idx * window + w;
    OriginSlot &o = os->slots[slotIdx];
    o.dest = m->dests[w];
    o.target = m->from;
    o.signal = (uint16_t)(m->slotBase + w);
    o.seq = w + 1;
    if (o.seq <= (uint64_t)putsPerPair)
      issue(slotIdx);
    else
      slotDone();
  }
  CmiFree(msg);
}

static void doneHandler(void *msg) {
  CmiFree(msg);
  if (++CpvAccess(doneMsgs) < 2 * CmiNumPes())
    return;
  CmiPrintf("put_signal: %d processes x %d PEs, %d puts per pair, window %d, "
            "2 kinds, CmiIsPutSignalCapable %d: %ld signals checked on "
            "arrival in %.2f s, every signal PASS\n",
            CmiNumNodes(), CmiMyNodeSize(), putsPerPair, window,
            CmiIsPutSignalCapable(),
            (long)CmiNumPes() * numPeers() * putsPerPair,
            CmiWallTimer() - CpvAccess(startTime));
  CmiExit(0);
}

static char *allocRegistered(size_t bytes, CmiNcpyBuffer *reg) {
  char *p = (char *)malloc(bytes);
  memset(p, 0, bytes);
  *reg = CmiNcpyBuffer(p, bytes);
  return p;
}

static void mymain(int argc, char **argv) {
  exchangeIdx = CmiRegisterHandler(exchangeHandler);
  ackIdx = CmiRegisterHandler(ackHandler);
  landedIdx = CmiRegisterHandler(landedHandler);
  creditIdx = CmiRegisterHandler(creditHandler);
  doneIdx = CmiRegisterHandler(doneHandler);
  recheckIdx = CmiRegisterHandler(recheckHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1)
    putsPerPair = atoi(argv[1]);
  if (argc > 2)
    window = atoi(argv[2]);
  if (CmiNumPes() < 2 || putsPerPair < 1 || window < 1 ||
      window > kMaxWindow || CmiMyNodeSize() > kMaxRanks ||
      (long)numPeers() * window > CMI_PUT_SIGNAL_MAX + 1)
    CmiAbort("put_signal: needs 2 PEs or more, at least 1 put per pair, a "
             "window of 1 to %d, at most %d PEs per process, and fewer than "
             "65536 slots per PE\n", kMaxWindow, kMaxRanks);

  CpvInitialize(OriginState *, origin);
  CpvInitialize(int, doneMsgs);
  CpvInitialize(double, startTime);
  CpvAccess(doneMsgs) = 0;

  int me = CmiMyPe(), npeers = numPeers();
  TargetState *t = new TargetState;
  t->pe = me;
  t->nslots = npeers * window;
  t->slots = new Slot[t->nslots];
  for (int j = 0; j < t->nslots; j++) {
    Slot &sl = t->slots[j];
    sl.buf = allocRegistered(kSlotBytes, &sl.reg);
    memcpy(sl.buf, &kCleared, 8);
    sl.origin = peerPe(me, j / window);
    uint64_t first = j % window + 1;
    sl.expect.store(first <= (uint64_t)putsPerPair ? first : 0);
  }
  t->raw = allocRegistered(npeers * kRawBytes, &t->rawReg);
  t->arrivals = t->arrivalsByKind[0] = t->arrivalsByKind[1] = 0;
  targets[CmiMyRank()] = t;

  OriginState *os = new OriginState;
  os->slots = new OriginSlot[npeers * window];
  for (int j = 0; j < npeers * window; j++)
    os->slots[j].src = allocRegistered(kSlotBytes, &os->slots[j].srcReg);
  os->slotsDone = 0;
  os->raw = allocRegistered(npeers * kRawBytes, &os->rawReg);
  for (int i = 0; i < npeers; i++)
    for (size_t b = 0; b < kRawBytes; b++)
      os->raw[i * kRawBytes + b] = (char)rawPattern(me, b);
  CpvAccess(origin) = os;

  CmiSetDirectNcpyAckHandler(ncpyAckHandler);
  if (CmiMyRank() == 0) {
    kinds[0] = CmiRegisterRputSignalKind(signalHandler, &kindCtx[0]);
    kinds[1] = CmiRegisterRputSignalKind(signalHandler, &kindCtx[1]);
    if (me == 0)
      CmiPrintf("put_signal: CmiIsPutSignalCapable() = %d\n",
                CmiIsPutSignalCapable());
  }
  // every process has its kinds and every PE its slots before anyone puts
  CmiBarrier();
  CpvAccess(startTime) = CmiWallTimer();

  for (int i = 0; i < npeers; i++) {
    int o = peerPe(me, i);
    ExchangeMsg *m = (ExchangeMsg *)CmiAlloc(sizeof(ExchangeMsg));
    CmiSetHandler(m, exchangeIdx);
    m->from = me;
    m->slotBase = i * window;
    for (int w = 0; w < window; w++)
      m->dests[w] = t->slots[i * window + w].reg;
    m->raw = t->rawReg;
    CmiSyncSendAndFree(o, sizeof(ExchangeMsg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
