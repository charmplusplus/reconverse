// Bursts of sends between many PEs of several processes, to PEs that are busy
// from time to time, with every message accounted for.
//
// Every PE runs `rounds` rounds. In a round it sends `burst` messages, each to
// a PE picked at random and of a size picked from a table that has entries on
// both sides of the backend's eager limit, and then sends itself the message
// that starts its next round, so the scheduler runs in between. Every fourth
// round begins with a few milliseconds of spinning, and so does every 64th
// message received: a PE in such a spin receives nothing, the network layer
// stops accepting sends for its process, and the senders' backend has to hold
// them and deliver them later.
//
// A message carries its source PE, its destination PE and its number among the
// messages of that pair, and the receiver checks its size and contents and that
// it has not arrived before. When every PE has finished its rounds, PE 0 asks
// all PEs how many messages they have received until the sum is the number
// sent. If the sum stops growing for `wait` seconds the test fails: every PE
// then tells every other how many messages it sent it, each destination prints
// which numbers are missing, and each process raises SIGUSR2, on which the LCI
// backend prints its send queues.
//
// Usage: send_stress [rounds = 200] [burst = 16] [wait seconds = 15]
#include "converse.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static const int sizes[] = {64, 300, 1000, 4000, 8000, 9000, 20000, 70000};
static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

static int round_handler, data_handler, finished_handler, count_req_handler,
    count_reply_handler, dump_handler, sent_count_handler, exit_handler;
static long rounds = 200;
static int burst = 16;
static double wait_seconds = 15.0;

struct Msg {
  char core[CmiMsgHeaderSizeBytes];
  int src, dst, bytes;
  long seq;  // data: number within (src, dst); otherwise a count
};

struct PeState {
  unsigned long rng;
  long round = 0;
  long received = 0;
  std::vector<long> sent;                // by destination PE
  std::vector<std::vector<bool>> seen;   // by source PE, then seq
  int sentCountsSeen = 0;
};
CpvStaticDeclare(PeState *, st);

// PE 0
static int finished = 0, replies = 0, dumps = 0;
static long reply_sum = 0, last_sum = -1;
static double last_change, start_time;
static bool failing = false;
static std::atomic<bool> signalled{false};  // per process

static unsigned long nextRand(PeState *s) {
  s->rng = s->rng * 6364136223846793005UL + 1442695040888963407UL;
  return s->rng >> 33;
}

static unsigned char patternByte(int src, int dst, long seq, size_t i) {
  return (unsigned char)((i * 131u + (size_t)seq * 17u + src * 7u + dst) & 0xffu);
}

static Msg *newMsg(int handler, int bytes = sizeof(Msg)) {
  Msg *m = (Msg *)CmiAlloc(bytes);
  CmiSetHandler(m, handler);
  m->src = CmiMyPe();
  m->bytes = bytes;
  return m;
}

static void spinFor(double seconds) {
  auto end = std::chrono::steady_clock::now() +
             std::chrono::duration<double>(seconds);
  volatile unsigned long spin = 0;
  while (std::chrono::steady_clock::now() < end)
    spin = spin + 1;
}

static void roundHandler(void *vmsg) {
  PeState *s = CpvAccess(st);
  if (s->round == rounds) {
    CmiFree(vmsg);
    CmiSyncSendAndFree(0, sizeof(Msg), newMsg(finished_handler));
    return;
  }
  if (s->round % 4 == (CmiMyPe() & 3))
    spinFor(0.001 + 0.001 * (nextRand(s) % 3));
  s->round++;
  for (int i = 0; i < burst; i++) {
    int dst = nextRand(s) % CmiNumPes();
    int bytes = sizes[nextRand(s) % nsizes];
    Msg *m = newMsg(data_handler, bytes);
    m->dst = dst;
    m->seq = s->sent[dst]++;
    char *p = (char *)m;
    for (size_t j = sizeof(Msg); j < (size_t)bytes; j++)
      p[j] = (char)patternByte(m->src, dst, m->seq, j);
    CmiSyncSendAndFree(dst, bytes, m);
  }
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Msg), vmsg);
}

static void dataHandler(void *vmsg) {
  PeState *s = CpvAccess(st);
  Msg *m = (Msg *)vmsg;
  if (m->dst != CmiMyPe() || m->src < 0 || m->src >= CmiNumPes() || m->seq < 0)
    CmiAbort("send_stress: PE %d got a message for PE %d from PE %d, number "
             "%ld\n", CmiMyPe(), m->dst, m->src, m->seq);
  std::vector<bool> &seen = s->seen[m->src];
  if ((size_t)m->seq >= seen.size()) seen.resize(m->seq + 1, false);
  if (seen[m->seq])
    CmiAbort("send_stress: message %ld from PE %d to PE %d arrived twice\n",
             m->seq, m->src, m->dst);
  seen[m->seq] = true;
  const char *p = (const char *)m;
  for (size_t j = sizeof(Msg); j < (size_t)m->bytes; j++)
    if ((unsigned char)p[j] != patternByte(m->src, m->dst, m->seq, j))
      CmiAbort("send_stress: message %ld from PE %d to PE %d (%d bytes): byte "
               "%zu is wrong\n", m->seq, m->src, m->dst, m->bytes, j);
  CmiFree(m);
  if (++s->received % 64 == 0) spinFor(0.0005);
}

// PE 0: ask every PE for its count
static void askCounts(void *, double) {
  replies = 0;
  reply_sum = 0;
  CmiSyncBroadcastAllAndFree(sizeof(Msg), newMsg(count_req_handler));
}

static void finishedHandler(void *msg) {
  CmiFree(msg);
  if (++finished == CmiNumPes()) {
    last_change = CmiWallTimer();
    askCounts(nullptr, 0);
  }
}

static void countReqHandler(void *msg) {
  CmiFree(msg);
  Msg *m = newMsg(count_reply_handler);
  m->seq = CpvAccess(st)->received;
  CmiSyncSendAndFree(0, sizeof(Msg), m);
}

static void failNow(void *, double) {
  CmiAbort("send_stress: FAIL: %ld of %ld messages arrived\n", last_sum,
           (long)CmiNumPes() * rounds * burst);
}

static void countReplyHandler(void *vmsg) {
  reply_sum += ((Msg *)vmsg)->seq;
  CmiFree(vmsg);
  if (++replies < CmiNumPes() || failing) return;
  long total = (long)CmiNumPes() * rounds * burst;
  if (reply_sum == total) {
    CmiPrintf("send_stress: %d PEs in %d processes, %ld messages, %.2f s: "
              "PASS\n", CmiNumPes(), CmiNumNodes(), total,
              CmiWallTimer() - start_time);
    CmiSyncBroadcastAllAndFree(sizeof(Msg), newMsg(exit_handler));
    return;
  }
  double now = CmiWallTimer();
  if (reply_sum != last_sum) {
    last_sum = reply_sum;
    last_change = now;
  }
  if (now - last_change > wait_seconds) {
    failing = true;
    CmiPrintf("send_stress: %ld of %ld messages arrived and none for %.0f s; "
              "asking every PE what is missing\n", reply_sum, total,
              wait_seconds);
    CmiSyncBroadcastAllAndFree(sizeof(Msg), newMsg(dump_handler));
    CcdCallFnAfter(failNow, nullptr, 5000);
    return;
  }
  CcdCallFnAfter(askCounts, nullptr, 100);
}

// every PE: tell each PE how many messages this one sent it
static void dumpHandler(void *msg) {
  CmiFree(msg);
#ifndef _WIN32
  if (!signalled.exchange(true)) raise(SIGUSR2);
#endif
  PeState *s = CpvAccess(st);
  for (int d = 0; d < CmiNumPes(); d++) {
    Msg *m = newMsg(sent_count_handler);
    m->seq = s->sent[d];
    CmiSyncSendAndFree(d, sizeof(Msg), m);
  }
}

static void sentCountHandler(void *vmsg) {
  PeState *s = CpvAccess(st);
  Msg *m = (Msg *)vmsg;
  std::vector<bool> &seen = s->seen[m->src];
  long missing = 0;
  char list[256] = "";
  size_t len = 0;
  for (long q = 0; q < m->seq; q++)
    if ((size_t)q >= seen.size() || !seen[q]) {
      if (missing++ < 12)
        len += snprintf(list + len, sizeof(list) - len, " %ld", q);
    }
  if (missing)
    CmiPrintf("send_stress: MISSING PE %d (process %d) -> PE %d (process %d): "
              "%ld of %ld, numbers%s%s\n", m->src, CmiNodeOf(m->src), CmiMyPe(),
              CmiMyNode(), missing, m->seq, list, missing > 12 ? " ..." : "");
  CmiFree(m);
}

static void exitHandler(void *msg) {
  CmiFree(msg);
  CsdExitScheduler();
}

static void start(int argc, char **argv) {
  CpvInitialize(PeState *, st);
  PeState *s = new PeState;
  CpvAccess(st) = s;
  round_handler = CmiRegisterHandler(roundHandler);
  data_handler = CmiRegisterHandler(dataHandler);
  finished_handler = CmiRegisterHandler(finishedHandler);
  count_req_handler = CmiRegisterHandler(countReqHandler);
  count_reply_handler = CmiRegisterHandler(countReplyHandler);
  dump_handler = CmiRegisterHandler(dumpHandler);
  sent_count_handler = CmiRegisterHandler(sentCountHandler);
  exit_handler = CmiRegisterHandler(exitHandler);
  argc = CmiGetArgc(argv);
  if (argc > 1) rounds = atol(argv[1]);
  if (argc > 2) burst = atoi(argv[2]);
  if (argc > 3) wait_seconds = atof(argv[3]);
  s->rng = 88172645463325252UL + 1000003UL * CmiMyPe();
  s->sent.assign(CmiNumPes(), 0);
  s->seen.resize(CmiNumPes());
  // every PE of every process has its handlers before anyone sends
  CmiBarrier();
  if (CmiMyPe() == 0) start_time = CmiWallTimer();
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Msg), newMsg(round_handler));
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, start, 0, 0);
  return 0;
}
