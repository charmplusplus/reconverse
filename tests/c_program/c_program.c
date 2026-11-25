/* A Converse program written in C: compiled by the C compiler, it checks that
   the C-callable API in converse.h links from C (reconverse issue #284). */
#include "converse.h"
#include "conv-region.h"

typedef struct {
  CmiMessageHeader header;
  int hops;
} RingMsg;

static int ringHandler;
static CmiNodeLock countLock;
static int handled = 0; /* messages handled in this process, under countLock */

static void sendRing(int dest, int hops) {
  RingMsg *msg = (RingMsg *)CmiAlloc(sizeof(RingMsg));
  CmiSetHandler(msg, ringHandler);
  msg->hops = hops;
  CmiSyncSendAndFree(dest, sizeof(RingMsg), msg);
}

static void ringHandlerFn(void *vmsg) {
  RingMsg *msg = (RingMsg *)vmsg;
  int hops = msg->hops;
  CmiFree(msg);

  CmiLock(countLock);
  handled++;
  CmiUnlock(countLock);

  if (hops == CmiNumPes()) {
    /* The message has gone once round every PE and is back on PE 0. */
    CmiLock(countLock);
    CmiPrintf("c_program: ring of %d PEs complete (%d handled here)\n",
              CmiNumPes(), handled);
    CmiUnlock(countLock);
    CmiExit(0);
    return;
  }
  sendRing((CmiMyPe() + 1) % CmiNumPes(), hops + 1);
}

/* Regions and counters (conv-region.h) link from C too. */
static void regionCheck(void) {
  CmiCounter c = CmiCounterCreate(2, CmiCounterActionNone(), 0);
  CmiTarget t = CmiTargetMake(NULL, CMI_PIECE_ALL, c);
  CmiAttachment a = CmiAttachTarget(&t);
  if (CmiSignal(a) != CMI_REGION_OK || CmiCounterValue(c) != 1)
    CmiAbort("c_program: a signal left the counter at %d, not 1\n",
             (int)CmiCounterValue(c));
  CmiDetachTarget(a);
  CmiCounterDestroy(c, 0, NULL, NULL);
}

static void start(int argc, char **argv) {
  (void)argc;
  (void)argv;
  ringHandler = CmiRegisterHandler(ringHandlerFn);
  if (CmiMyRank() == 0)
    countLock = CmiCreateLock();
  CmiNodeBarrier();
  if (CmiMyPe() == 0) {
    regionCheck();
    sendRing(CmiNumPes() > 1 ? 1 : 0, 1);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, start, 0, 0);
  return 0;
}
