/* ipc_order: a burst of messages through the pool must come out in order.
 *
 * The pool's receive queue is a linked list in the destination's segment, and
 * it is the only thing between a sender's CmiSyncSendAndFree and the
 * destination's handler. A list that hands back the block it was given last
 * delivers a sender's messages backwards, and -- worse -- leaves the oldest
 * block sitting at the bottom until the queue drains all the way, which under
 * steady inbound traffic may be a long time.
 *
 * So: PE 1 stalls, PE 0 blasts a burst at it, and PE 1 then checks that the
 * burst arrives in the order it was sent. The stall is what makes this
 * deterministic -- it guarantees the whole burst is sitting in the queue
 * before a single block is taken out of it, which is exactly the backlog that
 * a newest-first queue mishandles. Without it a receiver keeping pace would
 * only see the occasional pair swapped.
 *
 * Converse promises nothing about ordering between PEs, and this test does not
 * claim it does: it only asserts the order when it can see that the pool
 * carried every message, which it checks through the delivery counter. A run
 * with the pool off reports what it saw and fails only if a message is lost,
 * duplicated or corrupted -- the backend makes no ordering promise either.
 *
 * Several PEs per process work too: each PE has its own receive queue in its
 * process's segment and is the only one that takes from it, so a block never
 * reaches its destination by a second hop through another PE. With k PEs per
 * process, rank r of process 0 sends its burst to rank r of process 1, and
 * each of the k receivers checks its own burst.
 */
#include <converse.h>
#include <stdio.h>
#include <string.h>

// Long enough that a newest-first queue is unmistakable rather than unlucky,
// and small enough that the default pool holds the whole burst with room to
// spare (each block is rounded up to a 128-byte bin, so this is ~0.5 MiB of
// an 8 MiB pool).
#define BURST 4096

// How long PE 1 refuses to look at its queue, so the whole burst piles up in
// it first.
#define STALL_SECONDS 0.25

struct SeqMsg {
  CmiMessageHeader header;
  int seq;
  int pattern; // derived from seq, so a torn or stale block is caught too
};

struct DoneMsg {
  CmiMessageHeader header;
  int inversions;    // deliveries that were not the next one expected
  int firstExpected; // the first such delivery: what was due...
  int firstGot;      // ...and what came
  int whenFirstSent; // where in the burst message 0 ended up (1 = first out)
  int poolRecvd;     // how many of the burst the pool delivered
  int receiver;      // the PE that sends this report
};

CpvDeclare(int, seqHIdx);
CpvDeclare(int, doneHIdx);
CpvDeclare(int, exitHIdx);

// A receiver's tally of its burst.
CpvDeclare(int, received);
CpvDeclare(int, expected);
CpvDeclare(int, inversions);
CpvDeclare(int, firstExpected);
CpvDeclare(int, firstGot);
CpvDeclare(int, whenFirstSent);
CpvDeclare(long, recvBefore);
// PE 0: receivers that have reported
CpvDeclare(int, doneCount);

static int expectIpc;

static int patternFor(int seq) { return 0x5eed0000 ^ (seq * 2654435761u); }

// ------------------------------------------------------------- handlers ---

static void seqHandler(void *vmsg) {
  SeqMsg *m = (SeqMsg *)vmsg;
  const int seq = m->seq;
  const int pattern = m->pattern;
  CmiFree(vmsg);

  if (seq < 0 || seq >= BURST)
    CmiAbort("ipc_order: PE %d got sequence number %d, outside the burst",
             CmiMyPe(), seq);
  if (pattern != patternFor(seq))
    CmiAbort("ipc_order: PE %d got a corrupt message: sequence %d carried "
             "pattern 0x%x, expected 0x%x",
             CmiMyPe(), seq, (unsigned)pattern, (unsigned)patternFor(seq));

  CpvAccess(received)++;
  if (seq == 0)
    CpvAccess(whenFirstSent) = CpvAccess(received);

  if (seq != CpvAccess(expected)) {
    if (CpvAccess(inversions) == 0) {
      CpvAccess(firstExpected) = CpvAccess(expected);
      CpvAccess(firstGot) = seq;
    }
    CpvAccess(inversions)++;
  }
  // Track the next one due by what actually arrived, so one displaced message
  // counts once instead of making every later one look wrong.
  CpvAccess(expected) = seq + 1;

  if (CpvAccess(received) < BURST)
    return;

  DoneMsg *d = (DoneMsg *)CmiAlloc(sizeof(DoneMsg));
  CmiSetHandler(d, CpvAccess(doneHIdx));
  d->inversions = CpvAccess(inversions);
  d->firstExpected = CpvAccess(firstExpected);
  d->firstGot = CpvAccess(firstGot);
  d->whenFirstSent = CpvAccess(whenFirstSent);
  d->poolRecvd = (int)(CmiIpcMessagesReceived() - CpvAccess(recvBefore));
  d->receiver = CmiMyPe();
  CmiSyncSendAndFree(0, sizeof(DoneMsg), d);
}

static void doneHandler(void *vmsg) {
  DoneMsg *d = (DoneMsg *)vmsg;
  const int inversions = d->inversions;
  const int firstExpected = d->firstExpected;
  const int firstGot = d->firstGot;
  const int whenFirstSent = d->whenFirstSent;
  const int poolRecvd = d->poolRecvd;
  const int receiver = d->receiver;
  CmiFree(vmsg);

  // The pool is only known to have carried the whole burst if it delivered
  // that many messages; anything less means some of it fell back to the
  // backend, and a burst split across two routes says nothing about either.
  const int allThroughPool = (poolRecvd >= BURST);

  CmiPrintf("ipc_order: PE %d: %d messages, %d delivered by the pool, %d out "
            "of order, first of the burst came out %d%s\n",
            receiver, BURST, poolRecvd, inversions, whenFirstSent,
            allThroughPool ? "" : " (order not checked: mixed routes)");

  if (expectIpc && !allThroughPool)
    CmiAbort("ipc_order: +expect-ipc, but only %d of %d messages came through "
             "the pool. The rest fell back to the backend, so this run did not "
             "test the pool's queue at all.",
             poolRecvd, BURST);

  if (allThroughPool && inversions != 0)
    CmiAbort("ipc_order: the pool delivered %d of %d messages out of the order "
             "they were sent (first: expected %d, got %d), and the first "
             "message sent came out %d of %d. A receive queue that hands back "
             "the newest block reverses a sender's messages and leaves the "
             "oldest one waiting for the queue to drain.",
             inversions, BURST, firstExpected, firstGot, whenFirstSent, BURST);

  // Stop once every receiver has reported
  if (++CpvAccess(doneCount) < CmiNumPes() / 2)
    return;
  void *stop = CmiAlloc(sizeof(CmiMessageHeader));
  CmiSetHandler(stop, CpvAccess(exitHIdx));
  CmiSyncBroadcastAllAndFree(sizeof(CmiMessageHeader), stop);
}

static void exitHandler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

// ------------------------------------------------------------------ main ---

static void ipcOrderInit(int argc, char **argv) {
  expectIpc = CmiGetArgFlagDesc(argv, "+expect-ipc",
                                "fail unless the pool carried the burst");

  CpvInitialize(int, seqHIdx);
  CpvAccess(seqHIdx) = CmiRegisterHandler((CmiHandler)seqHandler);
  CpvInitialize(int, doneHIdx);
  CpvAccess(doneHIdx) = CmiRegisterHandler((CmiHandler)doneHandler);
  CpvInitialize(int, exitHIdx);
  CpvAccess(exitHIdx) = CmiRegisterHandler((CmiHandler)exitHandler);

  CpvInitialize(int, received);
  CpvInitialize(int, expected);
  CpvInitialize(int, inversions);
  CpvInitialize(int, firstExpected);
  CpvInitialize(int, firstGot);
  CpvInitialize(int, whenFirstSent);
  CpvInitialize(long, recvBefore);
  CpvInitialize(int, doneCount);
  CpvAccess(doneCount) = 0;
  CpvAccess(received) = CpvAccess(expected) = CpvAccess(inversions) = 0;
  CpvAccess(firstExpected) = CpvAccess(firstGot) = -1;
  CpvAccess(whenFirstSent) = -1;
  CpvAccess(recvBefore) = CmiIpcMessagesReceived();

  if (CmiNumNodes() != 2 || CmiNumPes() != 2 * CmiMyNodeSize())
    CmiAbort("ipc_order needs 2 processes on one host with the same number "
             "of PEs each. Run it as: <launcher> -n 2 ./reconverse_ipc_order "
             "+pe 2 +ipc +expect-ipc (or +pe 2k for k PEs per process)");

  if (expectIpc && !CmiIpcEnabled())
    CmiAbort("ipc_order: PE %d: +expect-ipc, but the shared-memory pool is "
             "not up, so there is no pool queue to test.",
             CmiMyPe());

  if (CmiMyPe() == 0)
    CmiPrintf("ipc_order: %d messages of %d bytes, pool %s\n", BURST,
              (int)sizeof(SeqMsg), CmiIpcEnabled() ? CmiIpcImplName() : "off");

  // Every PE has to be ready for the burst, and every receiver's counter has
  // to be taken, before any of it is sent.
  CmiBarrier();

  // Process 1's PEs receive; rank r of process 0 sends to rank r of process 1
  const int k = CmiMyNodeSize();
  if (CmiMyNode() == 1) {
    // Stay out of the scheduler while PE 0 sends, so the whole burst is
    // queued before any of it is taken out.
    const double until = CmiWallTimer() + STALL_SECONDS;
    while (CmiWallTimer() < until)
      ;
    return;
  }

  for (int seq = 0; seq < BURST; seq++) {
    SeqMsg *m = (SeqMsg *)CmiAlloc(sizeof(SeqMsg));
    CmiSetHandler(m, CpvAccess(seqHIdx));
    m->seq = seq;
    m->pattern = patternFor(seq);
    CmiSyncSendAndFree(k + CmiMyRank(), sizeof(SeqMsg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)ipcOrderInit);
  return 0;
}
