/* Exercises the persistent communication interface.
 *
 * The test runs a sequence of phases, separated by a message barrier so that
 * one phase is fully drained before the next starts:
 *
 *   1. Pipelined burst. Every PE fires a long run of sends, of varying sizes,
 *      down two channels: one set up by the sender (CmiCreatePersistent) and
 *      one set up by the receiver (CmiCreateReceiverPersistent /
 *      CmiRegisterReceivePersistent). The sends go out back to back without
 *      returning to the scheduler, which pushes far more messages at a channel
 *      than it has buffers and exercises the flow control.
 *   2. Token ring. A token is passed around a ring many times, and each hop
 *      forwards the received buffer itself rather than a fresh message, so a
 *      persistent receive buffer is used as the source of the next persistent
 *      send.
 *   3. Multicast. Each PE installs an array of handles, one per destination,
 *      and multicasts through CmiSyncListSendAndFree so that
 *      CmiUsePersistentHandle walks the array one entry per PE.
 *   4. Teardown. Channels are destroyed with CmiDestroyPersistent and ordinary
 *      sends are checked to still work, then CmiDestroyAllPersistent cleans up
 *      what is left.
 *
 * Every message carries a checkable byte pattern, and every phase checks that
 * it received exactly the messages it expected, exactly once each.
 */

#include <converse.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BYTES 8192
#define MIN_PAYLOAD 8

enum {
  CHANNEL_A = 0, // set up by the sender
  CHANNEL_B = 1, // set up by the receiver
  CHANNEL_MC = 2 // multicast handle array
};

typedef struct {
  char core[CmiMsgHeaderSizeBytes];
  int channel;
  int iter;
  int srcPE;
  int payloadLen;
} TestMsg;

typedef struct {
  char core[CmiMsgHeaderSizeBytes];
  int hops;
  int srcPE;
  int payloadLen;
} TokenMsg;

typedef struct {
  char core[CmiMsgHeaderSizeBytes];
  PersistentReq req;
} ReqMsg;

typedef struct {
  char core[CmiMsgHeaderSizeBytes];
  int phase;
} ControlMsg;

/* handlers */
CpvStaticDeclare(int, startPhaseHIdx);
CpvStaticDeclare(int, phaseDoneHIdx);
CpvStaticDeclare(int, recvReqHIdx);
CpvStaticDeclare(int, dataHIdx);
CpvStaticDeclare(int, tokenHIdx);
CpvStaticDeclare(int, mcastHIdx);
CpvStaticDeclare(int, plainHIdx);

/* configuration */
CpvStaticDeclare(int, nIters);
CpvStaticDeclare(int, nHops);
CpvStaticDeclare(int, nMcastIters);

/* channels */
CpvStaticDeclare(PersistentHandle, handleA);
CpvStaticDeclare(PersistentHandle, handleB);
CpvStaticDeclare(PersistentHandle *, handleMC);
CpvStaticDeclare(int *, allPes);

/* counters */
CpvStaticDeclare(int, recvCountA);
CpvStaticDeclare(int, recvCountB);
CpvStaticDeclare(int, recvCountMC);
CpvStaticDeclare(int, plainCount);
CpvStaticDeclare(char *, seenA);
CpvStaticDeclare(char *, seenB);
CpvStaticDeclare(int, reportsSeen); // PE 0 only
CpvStaticDeclare(int, phase);

static int nextPeA(void) { return (CmiMyPe() + 1) % CmiNumPes(); }
static int prevPeA(void) { return (CmiMyPe() + CmiNumPes() - 1) % CmiNumPes(); }
static int nextPeB(void) { return (CmiMyPe() + CmiNumPes() / 2) % CmiNumPes(); }
static int prevPeB(void) {
  return (CmiMyPe() + CmiNumPes() - CmiNumPes() / 2) % CmiNumPes();
}

static int payloadLenFor(int iter) {
  const int span = MAX_BYTES - (int)sizeof(TestMsg) - MIN_PAYLOAD;
  return MIN_PAYLOAD + (iter * 137) % span;
}

static void fillPayload(char *payload, int len, int srcPE, int iter) {
  for (int i = 0; i < len; i++)
    payload[i] = (char)(srcPE * 131 + iter * 17 + i * 7);
}

static void checkPayload(const char *payload, int len, int srcPE, int iter,
                         const char *what) {
  for (int i = 0; i < len; i++) {
    char expected = (char)(srcPE * 131 + iter * 17 + i * 7);
    if (payload[i] != expected)
      CmiAbort("[%d] %s: byte %d of the message from PE %d (iteration %d) is "
               "%d, expected %d\n",
               CmiMyPe(), what, i, srcPE, iter, (int)payload[i], (int)expected);
  }
}

/* ----------------------------- phase barrier ----------------------------- */

static void reportPhaseDone(void) {
  ControlMsg *msg = (ControlMsg *)CmiAlloc(sizeof(ControlMsg));
  msg->phase = CpvAccess(phase);
  CmiSetHandler(msg, CpvAccess(phaseDoneHIdx));
  CmiSyncSendAndFree(0, sizeof(ControlMsg), msg);
}

static void runPhase(int phase);

static void startPhaseHandler(void *env) {
  ControlMsg *msg = (ControlMsg *)env;
  int next = msg->phase;
  CmiFree(msg);
  CpvAccess(phase) = next;
  runPhase(next);
}

static void phaseDoneHandler(void *env) {
  CmiFree(env);
  CmiAssert(CmiMyPe() == 0);
  if (++CpvAccess(reportsSeen) < CmiNumPes())
    return;

  CpvAccess(reportsSeen) = 0;
  ControlMsg *msg = (ControlMsg *)CmiAlloc(sizeof(ControlMsg));
  msg->phase = CpvAccess(phase) + 1;
  CmiSetHandler(msg, CpvAccess(startPhaseHIdx));
  CmiSyncBroadcastAllAndFree(sizeof(ControlMsg), msg);
}

/* ------------------------------ phase 0: setup --------------------------- */

/* The sender-initiated channel needs nothing beyond the call; the
   receiver-initiated one needs the request to reach the sender first. */
static void setupChannels(void) {
  CpvAccess(handleA) = CmiCreatePersistent(nextPeA(), MAX_BYTES);

  PersistentReq req = CmiCreateReceiverPersistent(MAX_BYTES);
  ReqMsg *msg = (ReqMsg *)CmiAlloc(sizeof(ReqMsg));
  msg->req = req;
  CmiSetHandler(msg, CpvAccess(recvReqHIdx));
  CmiSyncSendAndFree(prevPeB(), sizeof(ReqMsg), msg);
}

static void recvReqHandler(void *env) {
  ReqMsg *msg = (ReqMsg *)env;
  CpvAccess(handleB) = CmiRegisterReceivePersistent(msg->req);
  CmiFree(msg);

  if (CpvAccess(handleB) == NULL)
    CmiAbort("[%d] CmiRegisterReceivePersistent returned a null handle\n",
             CmiMyPe());
  reportPhaseDone();
}

/* --------------------------- phase 1: burst send ------------------------- */

static void sendBurst(PersistentHandle *handle, int destPE, int channel) {
  for (int iter = 0; iter < CpvAccess(nIters); iter++) {
    int payloadLen = payloadLenFor(iter);
    int size = (int)sizeof(TestMsg) + payloadLen;

    TestMsg *msg = (TestMsg *)CmiAlloc(size);
    msg->channel = channel;
    msg->iter = iter;
    msg->srcPE = CmiMyPe();
    msg->payloadLen = payloadLen;
    fillPayload((char *)msg + sizeof(TestMsg), payloadLen, CmiMyPe(), iter);
    CmiSetHandler(msg, CpvAccess(dataHIdx));

    CmiUsePersistentHandle(handle, 1);
    CmiSyncSendAndFree(destPE, size, msg);
    CmiUsePersistentHandle(NULL, 0);
  }
}

static void dataHandler(void *env) {
  TestMsg *msg = (TestMsg *)env;

  int expectedSrc = (msg->channel == CHANNEL_A) ? prevPeA() : prevPeB();
  if (msg->srcPE != expectedSrc)
    CmiAbort("[%d] channel %d: message from PE %d, expected PE %d\n", CmiMyPe(),
             msg->channel, msg->srcPE, expectedSrc);
  if (msg->payloadLen != payloadLenFor(msg->iter))
    CmiAbort("[%d] channel %d: message %d has payload length %d, expected %d\n",
             CmiMyPe(), msg->channel, msg->iter, msg->payloadLen,
             payloadLenFor(msg->iter));
  checkPayload((char *)msg + sizeof(TestMsg), msg->payloadLen, msg->srcPE,
               msg->iter, "burst");

  char *seen =
      (msg->channel == CHANNEL_A) ? CpvAccess(seenA) : CpvAccess(seenB);
  if (msg->iter < 0 || msg->iter >= CpvAccess(nIters))
    CmiAbort("[%d] channel %d: iteration %d out of range\n", CmiMyPe(),
             msg->channel, msg->iter);
  if (seen[msg->iter])
    CmiAbort("[%d] channel %d: iteration %d delivered twice\n", CmiMyPe(),
             msg->channel, msg->iter);
  seen[msg->iter] = 1;

  int channel = msg->channel;
  CmiFree(msg); // releases the persistent buffer back to the sender

  int done = (channel == CHANNEL_A) ? ++CpvAccess(recvCountA)
                                    : ++CpvAccess(recvCountB);
  if (done == CpvAccess(nIters) && CpvAccess(recvCountA) == CpvAccess(nIters) &&
      CpvAccess(recvCountB) == CpvAccess(nIters))
    reportPhaseDone();
}

/* --------------------------- phase 2: token ring ------------------------- */

static void sendToken(TokenMsg *msg, int size) {
  CmiSetHandler(msg, CpvAccess(tokenHIdx));
  CmiUsePersistentHandle(&CpvAccess(handleB), 1);
  CmiSyncSendAndFree(nextPeB(), size, msg);
  CmiUsePersistentHandle(NULL, 0);
}

static void startToken(void) {
  int payloadLen = 512;
  int size = (int)sizeof(TokenMsg) + payloadLen;
  TokenMsg *msg = (TokenMsg *)CmiAlloc(size);
  msg->hops = 0;
  msg->srcPE = CmiMyPe();
  msg->payloadLen = payloadLen;
  fillPayload((char *)msg + sizeof(TokenMsg), payloadLen, CmiMyPe(), 0);
  sendToken(msg, size);
}

static void tokenHandler(void *env) {
  TokenMsg *msg = (TokenMsg *)env;

  checkPayload((char *)msg + sizeof(TokenMsg), msg->payloadLen, msg->srcPE,
               msg->hops, "token");

  if (msg->hops >= CpvAccess(nHops)) {
    CmiFree(msg);
    ControlMsg *done = (ControlMsg *)CmiAlloc(sizeof(ControlMsg));
    done->phase = CpvAccess(phase);
    CmiSetHandler(done, CpvAccess(phaseDoneHIdx));
    /* One report per token rather than per PE: the phase ends when every
       token has finished its trip. */
    CmiSyncSendAndFree(0, sizeof(ControlMsg), done);
    return;
  }

  /* Forward the buffer we were handed, so the next send reads straight out of
     a persistent receive buffer. */
  msg->hops++;
  fillPayload((char *)msg + sizeof(TokenMsg), msg->payloadLen, msg->srcPE,
              msg->hops);
  sendToken(msg, (int)sizeof(TokenMsg) + msg->payloadLen);
}

/* --------------------------- phase 3: multicast -------------------------- */

static void setupMulticast(void) {
  int npes = CmiNumPes();
  CpvAccess(handleMC) =
      (PersistentHandle *)malloc(npes * sizeof(PersistentHandle));
  CpvAccess(allPes) = (int *)malloc(npes * sizeof(int));
  for (int pe = 0; pe < npes; pe++) {
    CpvAccess(allPes)[pe] = pe;
    CpvAccess(handleMC)[pe] = CmiCreatePersistent(pe, MAX_BYTES);
  }
}

static void sendMulticasts(void) {
  int npes = CmiNumPes();
  for (int iter = 0; iter < CpvAccess(nMcastIters); iter++) {
    int payloadLen = payloadLenFor(iter);
    int size = (int)sizeof(TestMsg) + payloadLen;

    TestMsg *msg = (TestMsg *)CmiAlloc(size);
    msg->channel = CHANNEL_MC;
    msg->iter = iter;
    msg->srcPE = CmiMyPe();
    msg->payloadLen = payloadLen;
    fillPayload((char *)msg + sizeof(TestMsg), payloadLen, CmiMyPe(), iter);
    CmiSetHandler(msg, CpvAccess(mcastHIdx));

    /* One handle per destination, consumed in the order of the PE list. */
    CmiUsePersistentHandle(CpvAccess(handleMC), npes);
    CmiSyncListSendAndFree(npes, CpvAccess(allPes), size, msg);
    CmiUsePersistentHandle(NULL, 0);
  }
}

static void mcastHandler(void *env) {
  TestMsg *msg = (TestMsg *)env;

  if (msg->payloadLen != payloadLenFor(msg->iter))
    CmiAbort("[%d] multicast: message %d from PE %d has payload length %d, "
             "expected %d\n",
             CmiMyPe(), msg->iter, msg->srcPE, msg->payloadLen,
             payloadLenFor(msg->iter));
  checkPayload((char *)msg + sizeof(TestMsg), msg->payloadLen, msg->srcPE,
               msg->iter, "multicast");
  CmiFree(msg);

  if (++CpvAccess(recvCountMC) == CpvAccess(nMcastIters) * CmiNumPes())
    reportPhaseDone();
}

/* --------------------------- phase 4: teardown --------------------------- */

static void plainHandler(void *env) {
  CmiFree(env);
  if (++CpvAccess(plainCount) == CmiNumPes())
    reportPhaseDone();
}

static void destroyAndSendPlain(void) {
  CmiDestroyPersistent(CpvAccess(handleA));
  CmiDestroyPersistent(CpvAccess(handleB));
  CpvAccess(handleA) = NULL;
  CpvAccess(handleB) = NULL;

  /* Ordinary messaging has to be unaffected by the teardown. */
  for (int pe = 0; pe < CmiNumPes(); pe++) {
    ControlMsg *msg = (ControlMsg *)CmiAlloc(sizeof(ControlMsg));
    msg->phase = CpvAccess(phase);
    CmiSetHandler(msg, CpvAccess(plainHIdx));
    CmiSyncSendAndFree(pe, sizeof(ControlMsg), msg);
  }
}

/* ------------------------------- phase driver ---------------------------- */

static void runPhase(int phase) {
  switch (phase) {
  case 1:
    if (CmiMyPe() == 0)
      CmiPrintf("Phase 1: %d pipelined sends per channel\n", CpvAccess(nIters));
    sendBurst(&CpvAccess(handleA), nextPeA(), CHANNEL_A);
    sendBurst(&CpvAccess(handleB), nextPeB(), CHANNEL_B);
    break;

  case 2:
    if (CmiMyPe() == 0)
      CmiPrintf("Phase 2: token ring, %d hops per token\n", CpvAccess(nHops));
    startToken();
    break;

  case 3:
    if (CmiMyPe() == 0)
      CmiPrintf("Phase 3: %d multicasts through a handle array\n",
                CpvAccess(nMcastIters));
    setupMulticast();
    sendMulticasts();
    break;

  case 4:
    if (CmiMyPe() == 0)
      CmiPrintf("Phase 4: teardown\n");
    destroyAndSendPlain();
    break;

  case 5:
    if (CpvAccess(recvCountA) != CpvAccess(nIters) ||
        CpvAccess(recvCountB) != CpvAccess(nIters))
      CmiAbort("[%d] burst phase received %d/%d on channel A and %d/%d on "
               "channel B\n",
               CmiMyPe(), CpvAccess(recvCountA), CpvAccess(nIters),
               CpvAccess(recvCountB), CpvAccess(nIters));
    if (CpvAccess(recvCountMC) != CpvAccess(nMcastIters) * CmiNumPes())
      CmiAbort("[%d] multicast phase received %d of %d messages\n", CmiMyPe(),
               CpvAccess(recvCountMC), CpvAccess(nMcastIters) * CmiNumPes());

    CmiDestroyAllPersistent();
    free(CpvAccess(handleMC));
    free(CpvAccess(allPes));

    if (CmiMyPe() == 0)
      CmiPrintf("All persistent communication tests passed on %d PEs\n",
                CmiNumPes());
    CsdExitScheduler();
    break;

  default:
    CmiAbort("[%d] unknown phase %d\n", CmiMyPe(), phase);
  }
}

CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, startPhaseHIdx);
  CpvInitialize(int, phaseDoneHIdx);
  CpvInitialize(int, recvReqHIdx);
  CpvInitialize(int, dataHIdx);
  CpvInitialize(int, tokenHIdx);
  CpvInitialize(int, mcastHIdx);
  CpvInitialize(int, plainHIdx);

  CpvAccess(startPhaseHIdx) = CmiRegisterHandler((CmiHandler)startPhaseHandler);
  CpvAccess(phaseDoneHIdx) = CmiRegisterHandler((CmiHandler)phaseDoneHandler);
  CpvAccess(recvReqHIdx) = CmiRegisterHandler((CmiHandler)recvReqHandler);
  CpvAccess(dataHIdx) = CmiRegisterHandler((CmiHandler)dataHandler);
  CpvAccess(tokenHIdx) = CmiRegisterHandler((CmiHandler)tokenHandler);
  CpvAccess(mcastHIdx) = CmiRegisterHandler((CmiHandler)mcastHandler);
  CpvAccess(plainHIdx) = CmiRegisterHandler((CmiHandler)plainHandler);

  CpvInitialize(int, nIters);
  CpvInitialize(int, nHops);
  CpvInitialize(int, nMcastIters);
  CpvAccess(nIters) = 100;
  CpvAccess(nHops) = 50;
  CpvAccess(nMcastIters) = 20;
  CmiGetArgInt(argv, "-iters", &CpvAccess(nIters));
  CmiGetArgInt(argv, "-hops", &CpvAccess(nHops));
  CmiGetArgInt(argv, "-mcast", &CpvAccess(nMcastIters));

  CpvInitialize(PersistentHandle, handleA);
  CpvInitialize(PersistentHandle, handleB);
  CpvInitialize(PersistentHandle *, handleMC);
  CpvInitialize(int *, allPes);
  CpvAccess(handleA) = NULL;
  CpvAccess(handleB) = NULL;
  CpvAccess(handleMC) = NULL;
  CpvAccess(allPes) = NULL;

  CpvInitialize(int, recvCountA);
  CpvInitialize(int, recvCountB);
  CpvInitialize(int, recvCountMC);
  CpvInitialize(int, plainCount);
  CpvInitialize(int, reportsSeen);
  CpvInitialize(int, phase);
  CpvInitialize(char *, seenA);
  CpvInitialize(char *, seenB);
  CpvAccess(recvCountA) = 0;
  CpvAccess(recvCountB) = 0;
  CpvAccess(recvCountMC) = 0;
  CpvAccess(plainCount) = 0;
  CpvAccess(reportsSeen) = 0;
  CpvAccess(phase) = 0;
  CpvAccess(seenA) = (char *)calloc(CpvAccess(nIters), 1);
  CpvAccess(seenB) = (char *)calloc(CpvAccess(nIters), 1);

  if (CmiNumPes() < 2)
    CmiAbort("This test needs at least 2 PEs\n");

  if (CmiMyPe() == 0)
    CmiPrintf("Persistent communication test on %d PEs (%d nodes), buffer "
              "size %d bytes, %d buffers per channel\n",
              CmiNumPes(), CmiNumNodes(), MAX_BYTES, PERSIST_BUFFERS_NUM);

  CmiNodeBarrier();
  setupChannels();
  return 0;
}

int main(int argc, char *argv[]) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
