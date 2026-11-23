// Order and loss check for the PE thread queue (issue #271). Every message
// carries its sender and a per-sender sequence number, and PE 0 checks that
// each sender's messages arrive complete and in order, in three phases:
//   1. burst:  PE 0 blocks in a handler while every other PE of its process
//              pushes `burst` messages at it, so its queue fills far past
//              its first ring and grows several times under concurrent
//              producers before PE 0 takes anything;
//   2. self:   PE 0 pushes `self` messages to its own thread queue from one
//              handler (CmiPushPE), so the consumer itself makes it grow;
//   3. stream: every other PE sends `stream` messages while PE 0 takes them,
//              so the ring wraps many times with producers and the consumer
//              active together.
// Single process only (the phase 1 hand-off is a process-local counter).
//
// Usage: thread_queue_order +pe N [burst] [self] [stream]
#include "converse.h"
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

struct Msg {
  char core[CmiMsgHeaderSizeBytes];
  int phase;
  int sender;
  long seq;
};

static int msg_idx, burst_idx, stream_idx, self_idx;
static long counts[3] = {20000, 20000, 50000}; // per sender: burst, self, stream
static std::atomic<int> producers_done{0};
static std::vector<long> next_seq; // PE 0: next expected seq per sender
static long received = 0, expected = 0;
static int phase = 0;

static void sendOne(int dest, int ph, long seq) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->phase = ph;
  m->sender = CmiMyPe();
  m->seq = seq;
  CmiSetHandler(m, msg_idx);
  if (dest == CmiMyPe())
    CmiPushPE(CmiMyRank(), m); // the thread queue; a self CmiSyncSend would not use it
  else
    CmiSyncSendAndFree(dest, sizeof(Msg), m);
}

static void fail(const char *what, const Msg *m) {
  CmiPrintf("thread_queue_order: FAIL in phase %d: %s (sender %d, seq %ld, "
            "expected %ld)\n", phase + 1, what, m->sender, m->seq,
            next_seq[m->sender]);
  CmiAbort("thread_queue_order");
}

static void startPhase(int ph);

static void msgHandler(void *vm) {
  Msg *m = (Msg *)vm;
  if (m->phase != phase) fail("message from another phase", m);
  if (m->seq != next_seq[m->sender]) fail("out of order or lost", m);
  next_seq[m->sender]++;
  CmiFree(m);
  if (++received < expected) return;
  CmiPrintf("thread_queue_order: phase %d passed (%ld messages)\n", phase + 1,
            received);
  if (phase < 2) {
    startPhase(phase + 1);
  } else {
    CmiPrintf("thread_queue_order: PASS\n");
    CmiExit(0);
  }
}

// other PEs, phase 1: push the burst, then tell PE 0
static void burstHandler(void *msg) {
  CmiFree(msg);
  for (long i = 0; i < counts[0]; i++) sendOne(0, 0, i);
  producers_done.fetch_add(1, std::memory_order_release);
}

// other PEs, phase 3
static void streamHandler(void *msg) {
  CmiFree(msg);
  for (long i = 0; i < counts[2]; i++) sendOne(0, 2, i);
}

static void signalOthers(int handler) {
  for (int pe = 1; pe < CmiMyNodeSize(); pe++) {
    Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
    CmiSetHandler(m, handler);
    CmiSyncSendAndFree(pe, sizeof(Msg), m);
  }
}

// PE 0, phase 2: everything is pushed before any of it can be taken
static void selfHandler(void *msg) {
  CmiFree(msg);
  for (long i = 0; i < counts[1]; i++) sendOne(0, 1, i);
}

static void startPhase(int ph) {
  phase = ph;
  received = 0;
  for (long &s : next_seq) s = 0;
  int others = CmiMyNodeSize() - 1;
  if (ph == 0) {
    expected = others * counts[0];
    signalOthers(burst_idx);
    // take nothing until every burst is in the queue
    while (producers_done.load(std::memory_order_acquire) < others) {}
  } else if (ph == 1) {
    expected = counts[1];
    Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
    CmiSetHandler(m, self_idx);
    CmiPushPE(CmiMyRank(), m);
  } else {
    expected = others * counts[2];
    signalOthers(stream_idx);
  }
}

static void moduleInit(int argc, char **argv) {
  argc = CmiGetArgc(argv);
  msg_idx = CmiRegisterHandler(msgHandler);
  burst_idx = CmiRegisterHandler(burstHandler);
  stream_idx = CmiRegisterHandler(streamHandler);
  self_idx = CmiRegisterHandler(selfHandler);
  for (int i = 1; i < argc && i <= 3; i++) counts[i - 1] = atol(argv[i]);
  if (CmiNumNodes() != 1 || CmiMyNodeSize() < 2) {
    if (CmiMyPe() == 0)
      CmiPrintf("thread_queue_order: needs one process with at least 2 PEs\n");
    CmiExit(1);
    return;
  }
  // every PE registers its handlers before PE 0 starts anyone
  CmiNodeAllBarrier();
  if (CmiMyPe() == 0) {
    next_seq.assign(CmiMyNodeSize(), 0);
    startPhase(0);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
