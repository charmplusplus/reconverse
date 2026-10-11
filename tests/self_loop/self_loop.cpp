// A PE whose handler keeps sending to itself must not starve messages from
// other PEs. PE 0 loops: its handler re-sends the message to PE 0 at once, so
// PE 0 always has a self-sent message pending. After 200 ms, the last PE sends
// PE 0 a stop message. If self-sent messages take precedence over the queue
// that other PEs (and the network) feed, the stop message never runs and the
// test times out. Run with +pe 2 in one process (stop arrives through the
// per-PE queue) and with two processes (stop arrives through the network).
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

static int loop_idx, stop_idx;
static long loops;

struct Hdr {
  char core[CmiMsgHeaderSizeBytes];
};

static void loopHandler(void *msg) {
  loops++;
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Hdr), msg); // again, at once
}

static void stopHandler(void *msg) {
  CmiFree(msg);
  CmiPrintf("self_loop: stop message handled on PE %d after %ld self-sends\n",
            CmiMyPe(), loops);
  if (loops < 1) {
    CmiPrintf("self_loop: FAIL, the loop never ran\n");
    CmiAbort("self_loop: loop never ran");
  }
  CmiPrintf("self_loop: PASS\n");
  CmiExit(0);
}

static void sendStop(void *, double) {
  Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
  CmiSetHandler(m, stop_idx);
  CmiSyncSendAndFree(0, sizeof(Hdr), m);
}

static void moduleInit(int argc, char **argv) {
  loop_idx = CmiRegisterHandler(loopHandler);
  stop_idx = CmiRegisterHandler(stopHandler);
  if (CmiNumPes() < 2) {
    if (CmiMyPe() == 0) CmiPrintf("self_loop: needs at least 2 PEs\n");
    CmiExit(1);
  }
  if (CmiMyPe() == 0) {
    Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
    CmiSetHandler(m, loop_idx);
    CmiSyncSendAndFree(0, sizeof(Hdr), m);
  } else if (CmiMyPe() == CmiNumPes() - 1) {
    CcdCallFnAfter(sendStop, NULL, 200.0);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
