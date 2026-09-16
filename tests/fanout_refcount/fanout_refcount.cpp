// List send and multicast fan-out: delivery, ownership and within-process
// sharing (issue #219).
//
// Phase A is the regression test for the reason the fan-out is built the way
// it is. Sending one message to two PEs of the SAME remote process used to be
// two CmiSyncSends; the tempting optimization -- one buffer, one reference per
// destination -- mis-delivers, because the send path stamps header->destPE
// into the buffer before each issueAm and the receiving process reads that
// field back out of the received bytes. With one shared buffer both messages
// arrive at the PE named by the last stamp and the other destination gets
// none, for messages at or above LCI's eager threshold (~8 KB) and never
// below it. So phase A sends 8 KB (above) and 4 KB (below) and insists that
// each listed PE receives exactly one.
//
// Phase B: a list send to destinations spread over every process, with one
// process contributing several ranks and the others one each. Contents,
// exact arrival counts, and both ownership contracts: CmiSyncListSendFn
// leaves the caller's buffer intact with its reference count unchanged, and
// CmiFreeListSendFn drops exactly one reference (checked by holding a second
// one across the call).
//
// Phase C: the same list send with a nokeep payload. The destination process
// must hand ONE buffer to all its listed ranks, so those receivers report the
// same message pointer and a reference count between 1 and the number of
// listed ranks.
//
// Phase D: the same destinations through CmiEstablishGroup / CmiSyncMulticastFn
// and CmiFreeMulticastFn, which reach the same code by way of CmiLookupGroup.
#include "converse.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

enum Step {
  S_A_BIG,    // phase A, 8 KB, above the eager threshold
  S_A_SMALL,  // phase A, 4 KB, below it
  S_B_SYNC,   // CmiSyncListSendFn, caller keeps the buffer
  S_B_FREE,   // CmiFreeListSendFn, caller's reference is dropped
  S_C_NOKEEP, // nokeep payload, shared within each destination process
  S_D_MSYNC,  // CmiSyncMulticastFn
  S_D_MFREE,  // CmiFreeMulticastFn
  S_COUNT
};

static const char *stepName[S_COUNT] = {
    "A list send 8KB to two PEs of one process",
    "A list send 4KB to two PEs of one process",
    "B CmiSyncListSendFn across processes",
    "B CmiFreeListSendFn across processes",
    "C nokeep list send shared within each process",
    "D CmiSyncMulticastFn",
    "D CmiFreeMulticastFn"};

struct Payload {
  CmiMessageHeader header;
  int step;
  int len;
};

struct Ack {
  CmiMessageHeader header;
  int step;
  int pe;
  int bad;
  int ref;
  uint64_t ptr;
};

CpvDeclare(int, recvIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);

// PE 0 state
static int curStep = 0;
static int acksWanted = 0;
static int acksGot = 0;
static int arrivals[256];
static int expected[256];
static int badCount = 0;
static uint64_t ptrOf[256];
static int refOf[256];
static void *keptMsg = nullptr;

// destination lists
static int destA[2];
static int numDestA = 0;
static int destB[256];
static int numDestB = 0;
static int sharedGroupPes[2]; // two PEs of one remote process, phase C check
static int numSharedGroupPes = 0;
static CmiGroup group;

static const int bigSize = 8192;
static const int smallSize = 4096;

static unsigned char patternByte(int step, int i) {
  return (unsigned char)((step * 97 + i * 31 + 11) & 0xff);
}

static void *makePayload(int step, int size) {
  Payload *p = (Payload *)CmiAlloc(size);
  p->header.messageSize = size;
  CmiSetHandler(p, CpvAccess(recvIdx));
  p->step = step;
  p->len = size;
  unsigned char *body = (unsigned char *)p + sizeof(Payload);
  for (int i = 0; i < size - (int)sizeof(Payload); i++)
    body[i] = patternByte(step, i);
  return p;
}

static void recv_handler(void *vmsg) {
  Payload *p = (Payload *)vmsg;
  int bad = 0;
  if (p->len < (int)sizeof(Payload))
    bad = 1;
  else {
    unsigned char *body = (unsigned char *)p + sizeof(Payload);
    for (int i = 0; i < p->len - (int)sizeof(Payload); i++)
      if (body[i] != patternByte(p->step, i)) {
        bad = 1;
        break;
      }
  }
  int step = p->step;
  // Read, never write: a nokeep message may be shared with the other PEs of
  // this process right now.
  uint64_t ptr = (uint64_t)(uintptr_t)vmsg;
  int ref = CmiGetReference(vmsg);
  CmiFree(vmsg);

  Ack *a = (Ack *)CmiAlloc(sizeof(Ack));
  a->header.messageSize = sizeof(Ack);
  CmiSetHandler(a, CpvAccess(ackIdx));
  a->step = step;
  a->pe = CmiMyPe();
  a->bad = bad;
  a->ref = ref;
  a->ptr = ptr;
  CmiSyncSendAndFree(0, sizeof(Ack), a);
}

static void runStep(int step);
static void finish(void);

static void checkContents(void *msg, int step, int size, const char *what) {
  Payload *p = (Payload *)msg;
  if (p->step != step || p->len != size)
    CmiAbort("fanout_refcount: %s: %s header was modified", stepName[step],
             what);
  unsigned char *body = (unsigned char *)p + sizeof(Payload);
  for (int i = 0; i < size - (int)sizeof(Payload); i++)
    if (body[i] != patternByte(step, i))
      CmiAbort("fanout_refcount: %s: %s payload was modified", stepName[step],
               what);
}

static void checkStep(void) {
  int step = curStep;
  if (badCount)
    CmiAbort("fanout_refcount: %s: %d receivers saw a corrupted payload",
             stepName[step], badCount);
  int npes = CmiNumPes() < 256 ? CmiNumPes() : 256;
  for (int pe = 0; pe < npes; pe++)
    if (arrivals[pe] != expected[pe])
      CmiAbort("fanout_refcount: %s: pe %d received %d messages, expected %d",
               stepName[step], pe, arrivals[pe], expected[pe]);

  if (step == S_C_NOKEEP && numSharedGroupPes == 2) {
    // Both listed ranks of that process must have been handed the same buffer.
    int a = sharedGroupPes[0], b = sharedGroupPes[1];
    if (ptrOf[a] != ptrOf[b])
      CmiAbort("fanout_refcount: %s: pe %d and pe %d of one process got "
               "different buffers (%llx vs %llx); the nokeep payload was not "
               "shared",
               stepName[step], a, b, (unsigned long long)ptrOf[a],
               (unsigned long long)ptrOf[b]);
    if (refOf[a] < 1 || refOf[a] > 2 || refOf[b] < 1 || refOf[b] > 2)
      CmiAbort("fanout_refcount: %s: shared buffer reference counts %d and %d "
               "are outside 1..2",
               stepName[step], refOf[a], refOf[b]);
    CmiPrintf("[0] %s: shared buffer %llx, references seen %d and %d\n",
              stepName[step], (unsigned long long)ptrOf[a], refOf[a], refOf[b]);
  }

  if (keptMsg != nullptr) {
    // Both ownership contracts land here: the non-AndFree variants must leave
    // the caller's buffer alone, and the AndFree variants must have dropped
    // exactly the caller's one reference (this PE took a second one first).
    int ref = CmiGetReference(keptMsg);
    if (ref != 1)
      CmiAbort("fanout_refcount: %s: caller's buffer has %d references, "
               "expected 1",
               stepName[step], ref);
    checkContents(keptMsg, step, bigSize, "caller's buffer");
    CmiFree(keptMsg);
    keptMsg = nullptr;
  }

  CmiPrintf("[0] %s: ok\n", stepName[step]);
}

static void ack_handler(void *vmsg) {
  Ack *a = (Ack *)vmsg;
  if (a->step != curStep)
    CmiAbort("fanout_refcount: ack for step %d while running step %d", a->step,
             curStep);
  if (a->pe >= 0 && a->pe < 256) {
    arrivals[a->pe]++;
    ptrOf[a->pe] = a->ptr;
    refOf[a->pe] = a->ref;
  }
  if (a->bad)
    badCount++;
  CmiFree(a);
  if (++acksGot < acksWanted)
    return;
  checkStep();
  if (++curStep < S_COUNT)
    runStep(curStep);
  else
    finish();
}

static void beginStep(const int *dests, int ndests) {
  acksGot = 0;
  badCount = 0;
  acksWanted = ndests;
  memset(arrivals, 0, sizeof(arrivals));
  memset(expected, 0, sizeof(expected));
  for (int i = 0; i < ndests; i++)
    expected[dests[i]]++;
}

static void runStep(int step) {
  switch (step) {
  case S_A_BIG:
  case S_A_SMALL: {
    if (numDestA < 2) {
      CmiPrintf("[0] %s: needs 2 processes with 2 PEs each, skipped\n",
                stepName[step]);
      acksWanted = 0;
      if (++curStep < S_COUNT)
        runStep(curStep);
      else
        finish();
      return;
    }
    int size = (step == S_A_BIG) ? bigSize : smallSize;
    beginStep(destA, numDestA);
    void *m = makePayload(step, size);
    CmiSyncListSendFn(numDestA, destA, size, (char *)m);
    checkContents(m, step, size, "caller's buffer");
    if (CmiGetReference(m) != 1)
      CmiAbort("fanout_refcount: %s: CmiSyncListSendFn changed the buffer's "
               "reference count",
               stepName[step]);
    CmiFree(m);
    return;
  }
  case S_B_SYNC: {
    beginStep(destB, numDestB);
    void *m = makePayload(step, bigSize);
    CmiSyncListSendFn(numDestB, destB, bigSize, (char *)m);
    keptMsg = m; // checkStep verifies it is intact with one reference
    return;
  }
  case S_B_FREE: {
    beginStep(destB, numDestB);
    void *m = makePayload(step, bigSize);
    CmiReference(m); // stand in for another owner
    CmiFreeListSendFn(numDestB, destB, bigSize, (char *)m);
    keptMsg = m; // checkStep verifies exactly one reference was dropped
    return;
  }
  case S_C_NOKEEP: {
    beginStep(destB, numDestB);
    void *m = makePayload(step, bigSize);
    CMI_MSG_NOKEEP(m) = 1;
    CmiReference(m);
    CmiFreeListSendFn(numDestB, destB, bigSize, (char *)m);
    keptMsg = m;
    return;
  }
  case S_D_MSYNC: {
    beginStep(destB, numDestB);
    void *m = makePayload(step, bigSize);
    CmiSyncMulticastFn(group, bigSize, (char *)m);
    keptMsg = m;
    return;
  }
  case S_D_MFREE: {
    beginStep(destB, numDestB);
    void *m = makePayload(step, bigSize);
    CmiReference(m);
    CmiFreeMulticastFn(group, bigSize, (char *)m);
    keptMsg = m;
    return;
  }
  default:
    finish();
  }
}

static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void finish(void) {
  Payload *e = (Payload *)CmiAlloc(sizeof(Payload));
  e->header.messageSize = sizeof(Payload);
  CmiSetHandler(e, CpvAccess(exitIdx));
  e->step = 0;
  e->len = sizeof(Payload);
  CmiSyncBroadcastAllAndFree(sizeof(Payload), e);
}

static void buildDests(void) {
  const int nodes = CmiNumNodes(), nodeSize = CmiMyNodeSize();

  // Phase A: two PEs of one remote process.
  if (nodes >= 2 && nodeSize >= 2) {
    destA[0] = CmiNodeFirst(1);
    destA[1] = CmiNodeFirst(1) + 1;
    numDestA = 2;
  }

  // Phases B-D: one destination in this process, every rank of process 1, and
  // one rank of each remaining process -- a mix of one and several ranks per
  // process.
  numDestB = 0;
  if (nodes * nodeSize > 256 || nodes + nodeSize > 250)
    CmiAbort("fanout_refcount: built for at most 256 PEs");
  if (nodeSize >= 2)
    destB[numDestB++] = CmiNodeFirst(CmiMyNode()) + 1;
  for (int n = 0; n < nodes; n++) {
    if (n == CmiMyNode())
      continue;
    if (n == 1 && nodeSize >= 2) {
      for (int r = 0; r < nodeSize; r++)
        destB[numDestB++] = CmiNodeFirst(n) + r;
      sharedGroupPes[0] = CmiNodeFirst(n);
      sharedGroupPes[1] = CmiNodeFirst(n) + 1;
      numSharedGroupPes = 2;
    } else {
      destB[numDestB++] = CmiNodeFirst(n);
    }
  }
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, recvIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvAccess(recvIdx) = CmiRegisterHandler(recv_handler);
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  if (CmiMyPe() != 0)
    return;

  buildDests();
  if (numDestB == 0) {
    CmiPrintf("[0] fanout_refcount: no destinations besides pe 0, nothing to "
              "test\n");
    finish();
    return;
  }
  CmiPrintf("[0] fanout_refcount: %d processes x %d PEs, %d destinations\n",
            CmiNumNodes(), CmiMyNodeSize(), numDestB);
  group = CmiEstablishGroup(numDestB, destB);
  curStep = 0;
  runStep(curStep);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
