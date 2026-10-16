// Latency of node-level requests on a process whose PEs are busy with local
// work (issue #268).
//
// Every PE of process 1 runs work items back to back: a work item spins for
// a random time in [lo, hi] microseconds and then re-sends itself to its own
// PE, so each of those PEs always has local work queued. PE 0 (in process 0)
// is the client: it sends one request at a time to process 1 with
// CmiSyncNodeSendAndFree, so the request lands in that process's node queue
// and any of its PEs may serve it. The PE that does reads a node-shared
// table and replies to PE 0, which records the round trip and sends the next
// request. The reported figures are the round-trip latency percentiles.
//
// Mode `node` serves the request straight from the converse node queue.
// Mode `nodeprio` has the node-queue handler move the request into the node
// priority queue (CsdNodeEnqueueGeneral, priority 0) first, which is the path
// a Charm++ nodegroup message takes; it is then served from there.
//
// Usage: lcrun -n 2 mixed_grain +pe N [lo_us hi_us samples node|nodeprio]
//        (defaults: 1000 10000 200 node; needs at least 2 processes)
#include "converse.h"
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int work_idx, request_idx, serve_idx, reply_idx;
static double lo_us = 1000, hi_us = 10000;
static int samples = 200, warmup = 10;
static bool viaNodePrio = false;

// client state, touched only by PE 0
static std::vector<double> latencies;
static int sent = 0;

// node-shared data the requests read
static const int kTable = 1024;
static long table[kTable];

struct Work {
  char core[CmiMsgHeaderSizeBytes];
};

struct Request {
  char core[CmiMsgHeaderSizeBytes];
  double sentAt; // client clock
  int index;
  long answer;
};

static thread_local uint64_t rngState;
static double uniform01() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 7;
  rngState ^= rngState << 17;
  return (rngState >> 11) * (1.0 / 9007199254740992.0);
}

static void spin(double us) {
  double end = CmiWallTimer() + us * 1e-6;
  while (CmiWallTimer() < end) {
  }
}

static void workHandler(void *msg) {
  spin(lo_us + (hi_us - lo_us) * uniform01());
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Work), msg);
}

static void sendRequest() {
  Request *r = (Request *)CmiAlloc(sizeof(Request));
  CmiSetHandler(r, request_idx);
  r->index = sent++ % kTable;
  r->sentAt = CmiWallTimer();
  CmiSyncNodeSendAndFree(1, sizeof(Request), r);
}

static void serveHandler(void *msg) {
  Request *r = (Request *)msg;
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += table[(r->index + i) % kTable];
  r->answer = sum;
  CmiSetHandler(r, reply_idx);
  CmiSyncSendAndFree(0, sizeof(Request), r);
}

static void requestHandler(void *msg) {
  if (viaNodePrio) {
    CmiSetHandler(msg, serve_idx);
    CsdNodeEnqueueGeneral(msg, CQS_QUEUEING_FIFO, 0, NULL);
  } else {
    serveHandler(msg);
  }
}

static double percentile(const std::vector<double> &v, double p) {
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

static void replyHandler(void *msg) {
  Request *r = (Request *)msg;
  double us = (CmiWallTimer() - r->sentAt) * 1e6;
  CmiFree(r);
  if (sent > warmup) latencies.push_back(us);
  if ((int)latencies.size() < samples) {
    sendRequest();
    return;
  }
  std::sort(latencies.begin(), latencies.end());
  CmiPrintf("mixed_grain: %s, %d busy PEs, work %.0f-%.0f us, %d requests: "
            "latency us p50 %.1f p90 %.1f p99 %.1f max %.1f\n",
            viaNodePrio ? "nodeprio" : "node", CmiMyNodeSize(), lo_us, hi_us,
            samples, percentile(latencies, 0.5), percentile(latencies, 0.9),
            percentile(latencies, 0.99), latencies.back());
  CmiExit(0);
}

static void moduleInit(int argc, char **argv) {
  work_idx = CmiRegisterHandler(workHandler);
  request_idx = CmiRegisterHandler(requestHandler);
  serve_idx = CmiRegisterHandler(serveHandler);
  reply_idx = CmiRegisterHandler(replyHandler);
  if (argc > 1) lo_us = atof(argv[1]);
  if (argc > 2) hi_us = atof(argv[2]);
  if (argc > 3) samples = atoi(argv[3]);
  if (argc > 4) viaNodePrio = strcmp(argv[4], "nodeprio") == 0;
  rngState = 0x9e3779b97f4a7c15ULL * (CmiMyPe() + 1);
  if (CmiMyRank() == 0)
    for (int i = 0; i < kTable; i++) table[i] = i * 7 + CmiMyNode();

  if (CmiNumNodes() < 2) {
    if (CmiMyPe() == 0)
      CmiPrintf("mixed_grain: needs at least 2 processes\n");
    CmiExit(1);
    return;
  }
  // every PE registers its handlers before anyone sends
  CmiBarrier();
  if (CmiMyNode() == 1) {
    Work *w = (Work *)CmiAlloc(sizeof(Work));
    CmiSetHandler(w, work_idx);
    CmiSyncSendAndFree(CmiMyPe(), sizeof(Work), w);
  } else if (CmiMyPe() == 0) {
    sendRequest();
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
