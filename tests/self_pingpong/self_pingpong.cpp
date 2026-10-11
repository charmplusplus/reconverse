// Cost of one self-send through the scheduler.
//
// PE 0 sends one message to itself `count` times: the handler re-sends the
// message it received with CmiSyncSendAndFree, so the loop allocates, copies
// and prints nothing. The reported figure is the time per message, which is
// the enqueue plus whatever the scheduler loop does before it dequeues a
// self-sent message. Before the loop starts every other PE in the process
// sends PE 0 one message, so each of them is a registered producer on PE 0's
// thread queue; a scheduler whose check of that queue walks the producers
// shows that as a per-PE-count cost here.
//
// A second mode re-enqueues the message with CsdEnqueueGeneral (FIFO,
// priority 0) instead of sending it, i.e. straight into the PE's priority
// queue, the path a Charm++ local message takes; comparing the two modes
// separates the send path from what the scheduler loop costs before it
// reaches the priority queue.
//
// Usage: self_pingpong +pe N [count] [self|sched]
//        (count defaults to 1000000, mode to self)
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hello_idx, loop_idx;
static long count = 1000000, done = 0;
static int hellos = 0;
static bool viaSched = false; // sched mode: CsdEnqueueGeneral instead of a send
static double start;

struct Hdr {
  char core[CmiMsgHeaderSizeBytes];
};

static void loopHandler(void *msg) {
  if (++done < count) {
    if (viaSched) CsdEnqueueGeneral(msg, CQS_QUEUEING_FIFO, 0, NULL);
    else CmiSyncSendAndFree(CmiMyPe(), sizeof(Hdr), msg);
    return;
  }
  double t = CmiWallTimer() - start;
  CmiFree(msg);
  CmiPrintf("self_pingpong: %d PEs in this process, %ld %s, "
            "%.1f ns per message\n",
            CmiMyNodeSize(), count,
            viaSched ? "priority-queue enqueues" : "self-sends",
            t * 1e9 / count);
  CmiExit(0);
}

static void startLoop() {
  Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
  CmiSetHandler(m, loop_idx);
  start = CmiWallTimer();
  CmiSyncSendAndFree(0, sizeof(Hdr), m);
}

static void helloHandler(void *msg) {
  CmiFree(msg);
  if (++hellos == CmiMyNodeSize() - 1) startLoop();
}

static void moduleInit(int argc, char **argv) {
  hello_idx = CmiRegisterHandler(helloHandler);
  loop_idx = CmiRegisterHandler(loopHandler);
  if (argc > 1) count = atol(argv[1]);
  if (argc > 2) viaSched = strcmp(argv[2], "sched") == 0;
  if (CmiMyNode() != 0) return;
  if (CmiMyPe() == 0) {
    if (CmiMyNodeSize() == 1) startLoop();
  } else {
    Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
    CmiSetHandler(m, hello_idx);
    CmiSyncSendAndFree(0, sizeof(Hdr), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
