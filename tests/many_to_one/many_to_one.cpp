// Many-to-one throughput: every PE except 0 sends `count` messages to PE 0 as
// fast as it can, with no flow control, so PE 0's thread queue sees all the
// other PEs as concurrently active producers. PE 0 reports the time from the
// first message it handles to the last, and the resulting messages per second.
// This is the contention case for anything the producers share on the push
// path (a count, a tail pointer) and the producer-walk case for a consumer
// check that scans sub-queues.
//
// Usage: many_to_one +pe N [count per sender]   (count defaults to 200000)
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

static int msg_idx, go_idx;
static long count = 200000, received = 0, expected = 0;
static double start;

struct Hdr {
  char core[CmiMsgHeaderSizeBytes];
};

static void msgHandler(void *msg) {
  CmiFree(msg);
  if (received == 0) start = CmiWallTimer();
  if (++received < expected) return;
  double t = CmiWallTimer() - start;
  CmiPrintf("many_to_one: %d senders, %ld messages each, %.3f s, "
            "%.1f M msg/s, %.1f ns per message at the receiver\n",
            CmiMyNodeSize() - 1, count, t, expected / t / 1e6, t * 1e9 / expected);
  CmiExit(0);
}

static void goHandler(void *msg) {
  CmiFree(msg);
  for (long i = 0; i < count; i++) {
    Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
    CmiSetHandler(m, msg_idx);
    CmiSyncSendAndFree(0, sizeof(Hdr), m);
  }
}

static void moduleInit(int argc, char **argv) {
  // Bind this PE per +setcpuaffinity/+pemap; ConverseInit does not.
  CmiInitCPUAffinity(argv);
  argc = CmiGetArgc(argv);
  msg_idx = CmiRegisterHandler(msgHandler);
  go_idx = CmiRegisterHandler(goHandler);
  if (argc > 1) count = atol(argv[1]);
  if (CmiMyNode() != 0) return;
  expected = (long)(CmiMyNodeSize() - 1) * count;
  if (CmiMyNodeSize() < 2) {
    if (CmiMyPe() == 0) CmiPrintf("many_to_one: needs at least 2 PEs\n");
    CmiExit(1);
  }
  // every PE registers its handlers before PE 0 starts anyone
  CmiNodeAllBarrier();
  if (CmiMyPe() == 0) {
    for (int pe = 1; pe < CmiMyNodeSize(); pe++) {
      Hdr *m = (Hdr *)CmiAlloc(sizeof(Hdr));
      CmiSetHandler(m, go_idx);
      CmiSyncSendAndFree(pe, sizeof(Hdr), m);
    }
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
