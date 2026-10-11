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
// Usage: self_pingpong +pe N [count]   (count defaults to 1000000)
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

static int hello_idx, loop_idx;
static long count = 1000000, done = 0;
static int hellos = 0;
static double start;

struct Hdr {
  char core[CmiMsgHeaderSizeBytes];
};

static void loopHandler(void *msg) {
  if (++done < count) {
    CmiSyncSendAndFree(CmiMyPe(), sizeof(Hdr), msg);
    return;
  }
  double t = CmiWallTimer() - start;
  CmiFree(msg);
  CmiPrintf("self_pingpong: %d PEs in this process, %ld self-sends, "
            "%.1f ns per message\n",
            CmiMyNodeSize(), count, t * 1e9 / count);
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
