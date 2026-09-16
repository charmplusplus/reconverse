// Probe for reconverse issue #219: may one CmiAlloc'd buffer back several
// outstanding comm_backend::issueAm sends, with one reference per send and the
// LCI completion handler's CmiFree dropping them?
//
// Phase A: N sends of one shared buffer to the SAME remote PE. All sends stamp
//          the same header->destPE, so only the LCI-level question is tested.
// Phase B: the same buffer to two DIFFERENT remote PEs on one remote node
//          (ranks 0 and 1), which is what a refcounted CmiSyncListSend would
//          do. CmiSyncSendAndFree stamps header->destPE into the shared buffer
//          before each issueAm, so send k's destination field can be
//          overwritten by send k+1 before LCI has read the buffer.
//
// Run: lcrun -n 2 ./reconverse_fanout_probe +pe 4 [msgsize]
#include "converse.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Msg {
  CmiMessageHeader header;
  int phase;
  int seq;
  int len;
};

CpvDeclare(int, recvIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, pollIdx);
CpvDeclare(int, exitIdx);

static int msgSize = 65536;
static int phaseANumSends = 4;

// PE 0 state
static void *sharedMsg = nullptr;
static int acksWanted = 0;
static int acksGot = 0;
static int arrivalsPerPe[64];
static int curPhase = 0;
static int pollCount = 0;
static int payloadErrors = 0;

static unsigned char patternByte(int seq, int i) {
  return (unsigned char)((seq * 131 + i * 17 + 7) & 0xff);
}

static void *makeMsg(int phase, int seq, int size) {
  Msg *m = (Msg *)CmiAlloc(size);
  memset(m, 0, sizeof(Msg));
  m->header.messageSize = size;
  CmiSetHandler(m, CpvAccess(recvIdx));
  m->phase = phase;
  m->seq = seq;
  m->len = size;
  unsigned char *body = (unsigned char *)m + sizeof(Msg);
  for (int i = 0; i < size - (int)sizeof(Msg); i++)
    body[i] = patternByte(seq, i);
  return m;
}

static void recv_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  int bad = 0;
  if (m->len != msgSize)
    bad = 1;
  unsigned char *body = (unsigned char *)m + sizeof(Msg);
  for (int i = 0; i < m->len - (int)sizeof(Msg); i++)
    if (body[i] != patternByte(m->seq, i)) {
      bad = 1;
      break;
    }
  int phase = m->phase, seq = m->seq;
  CmiFree(m);

  Msg *ack = (Msg *)CmiAlloc(sizeof(Msg));
  memset(ack, 0, sizeof(Msg));
  ack->header.messageSize = sizeof(Msg);
  CmiSetHandler(ack, CpvAccess(ackIdx));
  ack->phase = phase;
  ack->seq = bad ? -1 : seq;
  ack->len = CmiMyPe();
  CmiSyncSendAndFree(0, sizeof(Msg), ack);
}

static void runPhaseB(void);
static void finish(void);

static void poll_handler(void *vmsg) {
  CmiFree(vmsg);
  int ref = CmiGetReference(sharedMsg);
  if (ref > 1 && ++pollCount < 2000000) {
    Msg *p = (Msg *)CmiAlloc(sizeof(Msg));
    memset(p, 0, sizeof(Msg));
    p->header.messageSize = sizeof(Msg);
    CmiSetHandler(p, CpvAccess(pollIdx));
    CmiSyncSendAndFree(0, sizeof(Msg), p);
    return;
  }
  CmiPrintf("[0] phase %c: refcount after all completions = %d (want 1), "
            "%d polls\n",
            'A' + curPhase, ref, pollCount);
  if (ref != 1)
    CmiPrintf("[0] phase %c: FAIL refcount did not return to 1\n",
              'A' + curPhase);
  // buffer must still hold the pattern the sender wrote
  Msg *m = (Msg *)sharedMsg;
  int bad = 0;
  unsigned char *body = (unsigned char *)m + sizeof(Msg);
  for (int i = 0; i < msgSize - (int)sizeof(Msg); i++)
    if (body[i] != patternByte(m->seq, i)) {
      bad = 1;
      break;
    }
  CmiPrintf("[0] phase %c: sender's buffer %s\n", 'A' + curPhase,
            bad ? "CORRUPTED" : "intact");
  CmiFree(sharedMsg);
  sharedMsg = nullptr;
  if (curPhase == 0)
    runPhaseB();
  else
    finish();
}

static void startPoll(void) {
  pollCount = 0;
  Msg *p = (Msg *)CmiAlloc(sizeof(Msg));
  memset(p, 0, sizeof(Msg));
  p->header.messageSize = sizeof(Msg);
  CmiSetHandler(p, CpvAccess(pollIdx));
  CmiSyncSendAndFree(0, sizeof(Msg), p);
}

static void ack_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  if (m->seq < 0)
    payloadErrors++;
  if (m->len >= 0 && m->len < 64)
    arrivalsPerPe[m->len]++;
  CmiFree(m);
  if (++acksGot < acksWanted)
    return;
  CmiPrintf("[0] phase %c: %d/%d acks, %d payload errors, arrivals:",
            'A' + curPhase, acksGot, acksWanted, payloadErrors);
  for (int i = 0; i < CmiNumPes(); i++)
    CmiPrintf(" pe%d=%d", i, arrivalsPerPe[i]);
  CmiPrintf("\n");
  startPoll();
}

static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void finish(void) {
  Msg *e = (Msg *)CmiAlloc(sizeof(Msg));
  memset(e, 0, sizeof(Msg));
  e->header.messageSize = sizeof(Msg);
  CmiSetHandler(e, CpvAccess(exitIdx));
  CmiSyncBroadcastAllAndFree(sizeof(Msg), e);
}

// Phase A: one buffer, N sends, all to the same remote PE.
static void runPhaseA(void) {
  curPhase = 0;
  acksGot = 0;
  payloadErrors = 0;
  memset(arrivalsPerPe, 0, sizeof(arrivalsPerPe));
  int dest = CmiNodeFirst(1);
  acksWanted = phaseANumSends;
  sharedMsg = makeMsg(0, 11, msgSize);
  CmiPrintf("[0] phase A: %d sends of one %d-byte buffer to pe %d\n",
            phaseANumSends, msgSize, dest);
  for (int i = 0; i < phaseANumSends; i++)
    CmiReference(sharedMsg); // one reference per send; PE 0 keeps its own
  for (int i = 0; i < phaseANumSends; i++)
    CmiSyncSendAndFree(dest, msgSize, sharedMsg);
}

// Phase B: one buffer, one send to each of two PEs on remote node 1.
static void runPhaseB(void) {
  curPhase = 1;
  acksGot = 0;
  payloadErrors = 0;
  memset(arrivalsPerPe, 0, sizeof(arrivalsPerPe));
  int d0 = CmiNodeFirst(1);
  int d1 = CmiNodeFirst(1) + 1;
  acksWanted = 2;
  sharedMsg = makeMsg(1, 22, msgSize);
  CmiPrintf("[0] phase B: one %d-byte buffer to pe %d and pe %d\n", msgSize, d0,
            d1);
  CmiReference(sharedMsg);
  CmiReference(sharedMsg);
  CmiSyncSendAndFree(d0, msgSize, sharedMsg);
  CmiSyncSendAndFree(d1, msgSize, sharedMsg);
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, recvIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, pollIdx);
  CpvInitialize(int, exitIdx);
  CpvAccess(recvIdx) = CmiRegisterHandler(recv_handler);
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(pollIdx) = CmiRegisterHandler(poll_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  if (argc > 1)
    msgSize = atoi(argv[1]);
  if (msgSize < (int)sizeof(Msg) + 16)
    msgSize = (int)sizeof(Msg) + 16;
  if (CmiNumNodes() < 2 || CmiMyNodeSize() < 2) {
    if (CmiMyPe() == 0)
      CmiPrintf("fanout_probe needs at least 2 processes with 2 PEs each\n");
    if (CmiMyPe() == 0)
      finish();
    return;
  }
  if (CmiMyPe() == 0)
    runPhaseA();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
