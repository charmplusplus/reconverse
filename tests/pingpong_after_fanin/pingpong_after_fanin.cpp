// Ping-pong (or one-sender stream) on PE 0's queue after every other PE has
// sent PE 0 one message. The fan-in phase makes every PE a registered producer
// on PE 0's thread queue (the producers stay registered for the rest of the
// run), so phase 2 measures delivery with one active producer among N-1
// registered ones: the case where a consumer that walks the producer list
// looking for non-empty sub-queues cannot stop early.
//
// Usage: pingpong_after_fanin +pe N [iters] [pp|stream] [nofanin]
//   pp:      PE 0 and PE 1 exchange `iters` 16-byte messages; PE 0 prints the
//            one-way latency (round trip / 2).
//   stream:  PE 1 sends PE 0 `iters` 16-byte messages back to back; PE 0
//            prints the time per message from the first to the last arrival.
//   nofanin: skip the fan-in phase (control).
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kPayload = 16, kWarmup = 1000 };

static int fanin_idx, ping_idx, pong_idx, go_idx, stream_idx;
static long iters = 200000;
static bool stream_mode = false, fanin = true;
static long fanin_seen = 0, count = 0;
static double start;

static void *newMsg(int handler) {
  void *m = CmiAlloc(CmiMsgHeaderSizeBytes + kPayload);
  CmiSetHandler(m, handler);
  return m;
}

static void startPhase2() {
  if (stream_mode) {
    CmiSyncSendAndFree(1, CmiMsgHeaderSizeBytes + kPayload, newMsg(go_idx));
  } else {
    CmiSyncSendAndFree(1, CmiMsgHeaderSizeBytes + kPayload, newMsg(ping_idx));
  }
}

static void faninHandler(void *msg) {
  CmiFree(msg);
  if (++fanin_seen == CmiMyNodeSize() - 1) startPhase2();
}

// PE 1: reply to every ping
static void pingHandler(void *msg) {
  CmiSetHandler(msg, pong_idx);
  CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes + kPayload, msg);
}

// PE 0: count round trips, time the ones after the warmup
static void pongHandler(void *msg) {
  ++count;
  if (count == kWarmup) start = CmiWallTimer();
  if (count < kWarmup + iters) {
    CmiSetHandler(msg, ping_idx);
    CmiSyncSendAndFree(1, CmiMsgHeaderSizeBytes + kPayload, msg);
    return;
  }
  CmiFree(msg);
  double t = CmiWallTimer() - start;
  CmiPrintf("pingpong_after_fanin: mode pp, %d PEs, fanin %s, %ld iters, "
            "%.1f ns one-way latency\n",
            CmiMyNodeSize(), fanin ? "yes" : "no", iters, t * 1e9 / iters / 2);
  CmiExit(0);
}

// PE 1: stream iters messages to PE 0 without waiting
static void goHandler(void *msg) {
  CmiFree(msg);
  for (long i = 0; i < iters; i++)
    CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes + kPayload, newMsg(stream_idx));
}

// PE 0: time from the first arrival to the last
static void streamHandler(void *msg) {
  CmiFree(msg);
  if (count == 0) start = CmiWallTimer();
  if (++count < iters) return;
  double t = CmiWallTimer() - start;
  CmiPrintf("pingpong_after_fanin: mode stream, %d PEs, fanin %s, %ld messages, "
            "%.1f ns per message at the receiver\n",
            CmiMyNodeSize(), fanin ? "yes" : "no", iters,
            t * 1e9 / (iters - 1));
  CmiExit(0);
}

static void moduleInit(int argc, char **argv) {
  fanin_idx = CmiRegisterHandler(faninHandler);
  ping_idx = CmiRegisterHandler(pingHandler);
  pong_idx = CmiRegisterHandler(pongHandler);
  go_idx = CmiRegisterHandler(goHandler);
  stream_idx = CmiRegisterHandler(streamHandler);
  CmiInitCPUAffinity(argv);
  argc = CmiGetArgc(argv);
  if (argc > 1) iters = atol(argv[1]);
  if (argc > 2) stream_mode = strcmp(argv[2], "stream") == 0;
  if (argc > 3) fanin = strcmp(argv[3], "nofanin") != 0;
  if (CmiNumNodes() != 1 || CmiMyNodeSize() < 2) {
    if (CmiMyPe() == 0)
      CmiPrintf("pingpong_after_fanin: needs one process with at least 2 PEs\n");
    CmiExit(1);
  }
  // every PE registers its handlers before anyone sends
  CmiNodeAllBarrier();
  if (fanin) {
    if (CmiMyPe() != 0)
      CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes + kPayload, newMsg(fanin_idx));
  } else if (CmiMyPe() == 0) {
    startPhase2();
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
