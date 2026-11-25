/***************************************************************
  CmiSyncSendAndFreeUrgent: the destination serves the message before what is
  already queued for it, and the mark applies to that one send.

  PE 0 queues kFillers messages for itself, each spinning kSpinUs. Then:
  - it sends itself an urgent message, which must run before any filler;
  - that message's handler sends the same buffer on to PE 0 with plain
    CmiSyncSendAndFree: it must run after every filler (the scheduler cleared
    the mark when it took the message, so the forward is an ordinary send);
  - every other PE, told to by an ordinary message, sends PE 0 an urgent
    reply, which must run before half the fillers have (the fillers take
    kFillers * kSpinUs, 50 ms, and a reply arrives within a few of them).
  Run with any number of PEs and processes; prints PASS on PE 0.
 ****************************************************************/

#include <converse.h>
#include <cstdlib>

static const int kFillers = 2000;
static const double kSpinUs = 25.0;

struct Msg {
  char core[CmiMsgHeaderSizeBytes];
  int src;
};

CpvDeclare(int, fillerHandler);
CpvDeclare(int, goHandler);
CpvDeclare(int, urgentSelfHandler);
CpvDeclare(int, forwardedHandler);
CpvDeclare(int, replyHandler);
CpvDeclare(int, exitHandler);

// PE 0's tallies
static int fillersRun = 0;
static int urgentSelfAt = -1;   // fillers run when the urgent self-send ran
static int forwardedAt = -1;    // fillers run when its forward ran
static int repliesSeen = 0;
static int worstReplyAt = -1;   // the most fillers run before an urgent reply

static Msg *newMsg(int handler) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->src = CmiMyPe();
  CmiSetHandler(m, handler);
  return m;
}

static void maybeDone() {
  if (fillersRun < kFillers || forwardedAt < 0 || repliesSeen < CmiNumPes() - 1) return;
  if (urgentSelfAt != 0)
    CmiAbort("urgent: FAIL: the urgent self-send ran after %d of %d fillers queued before it\n",
             urgentSelfAt, kFillers);
  if (forwardedAt != kFillers)
    CmiAbort("urgent: FAIL: the plain forward of a handled urgent message ran after %d of %d fillers "
             "queued before it: the mark outlived its send\n", forwardedAt, kFillers);
  if (worstReplyAt >= kFillers / 2)
    CmiAbort("urgent: FAIL: an urgent reply from another PE ran only after %d of %d fillers\n",
             worstReplyAt, kFillers);
  CmiPrintf("urgent PASS: %d PEs, %d processes; the urgent self-send ran first, its forward last, and "
            "%d urgent replies after at most %d of %d fillers\n",
            CmiNumPes(), CmiNumNodes(), repliesSeen, worstReplyAt, kFillers);
  Msg *m = newMsg(CpvAccess(exitHandler));
  CmiSyncBroadcastAllAndFree(sizeof(Msg), m);
}

static void handleFiller(Msg *m) {
  CmiFree(m);
  const double end = CmiWallTimer() + kSpinUs * 1e-6;
  while (CmiWallTimer() < end) {
  }
  fillersRun++;
  maybeDone();
}

static void handleUrgentSelf(Msg *m) {
  urgentSelfAt = fillersRun;
  CmiSetHandler(m, CpvAccess(forwardedHandler));
  CmiSyncSendAndFree(0, sizeof(Msg), m);
}

static void handleForwarded(Msg *m) {
  CmiFree(m);
  forwardedAt = fillersRun;
  maybeDone();
}

// Another PE, told by PE 0: reply urgent.
static void handleGo(Msg *m) {
  CmiFree(m);
  Msg *r = newMsg(CpvAccess(replyHandler));
  CmiSyncSendAndFreeUrgent(0, sizeof(Msg), r);
}

static void handleReply(Msg *m) {
  CmiFree(m);
  repliesSeen++;
  if (fillersRun > worstReplyAt) worstReplyAt = fillersRun;
  maybeDone();
}

static void handleExit(Msg *m) {
  CmiFree(m);
  CsdExitScheduler();
}

CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, fillerHandler);
  CpvInitialize(int, goHandler);
  CpvInitialize(int, urgentSelfHandler);
  CpvInitialize(int, forwardedHandler);
  CpvInitialize(int, replyHandler);
  CpvInitialize(int, exitHandler);
  CpvAccess(fillerHandler) = CmiRegisterHandler((CmiHandler)handleFiller);
  CpvAccess(goHandler) = CmiRegisterHandler((CmiHandler)handleGo);
  CpvAccess(urgentSelfHandler) = CmiRegisterHandler((CmiHandler)handleUrgentSelf);
  CpvAccess(forwardedHandler) = CmiRegisterHandler((CmiHandler)handleForwarded);
  CpvAccess(replyHandler) = CmiRegisterHandler((CmiHandler)handleReply);
  CpvAccess(exitHandler) = CmiRegisterHandler((CmiHandler)handleExit);

  if (CmiMyPe() == 0) {
    for (int i = 0; i < kFillers; i++) CmiSyncSendAndFree(0, sizeof(Msg), newMsg(CpvAccess(fillerHandler)));
    CmiSyncSendAndFreeUrgent(0, sizeof(Msg), newMsg(CpvAccess(urgentSelfHandler)));
    for (int pe = 1; pe < CmiNumPes(); pe++) CmiSyncSendAndFree(pe, sizeof(Msg), newMsg(CpvAccess(goHandler)));
  }
  return 0;
}

int main(int argc, char *argv[]) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
