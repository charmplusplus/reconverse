/* rdma_ipc_ack: a put's destination acknowledgement must not overtake the put.
 *
 * CmiIssueRput sends the payload over the network backend. Local completion of
 * that put only says the source buffer may be reused; it does not say the
 * bytes are visible in the destination process. The acknowledgement that tells
 * the destination "your buffer is filled" then travels as an ordinary Converse
 * message -- and under +ipc, to a peer process on this host, an ordinary
 * message goes through the shared-memory pool, which does not wait for the
 * network. Without the ordering scope in CommRputLocalHandler the ack can
 * arrive first and the receiver reads a buffer the data has not landed in yet.
 *
 * Two checks, on every round:
 *
 *   mechanism -- PE 0 counts the pool sends made by the one CmiSyncSendAndFree
 *                inside its acknowledgement handler. On the network RDMA path
 *                that count must not move: the runtime is supposed to hold
 *                that message on the backend, behind the put.
 *   symptom   -- PE 1 verifies every byte the moment that message is
 *                delivered. Its buffer is poisoned before each round, so a
 *                round that arrives early fails instead of passing on the
 *                bytes a previous round left behind.
 *
 * The mechanism check is deterministic; the symptom check is the race itself,
 * so it is run over sizes up to 4 MiB, to give an unordered ack the widest
 * window this test can offer it. On Delta over LCI's ofi backend the ack never
 * actually won that race -- 32 unordered rounds all read correct data -- so it
 * is the mechanism check that fails when the ordering is dropped. Keep both:
 * the mechanism check is what a regression trips on, and the symptom check is
 * what says the invariant holds on a fabric where local completion and remote
 * visibility are further apart.
 *
 * Needs two processes on one host: +ipc only carries messages between peer
 * processes that share memory, and a put within one process short-circuits to
 * a memcpy.
 */
#include "conv-rdma.h"
#include <converse.h>
#include <stdio.h>
#include <string.h>

// Big enough at the top end that a put is still in flight when an unordered
// ack would be delivered. The small sizes are there to keep the mechanism
// check honest for messages the pool would certainly have taken.
static const size_t msgSizes[] = {1024, 65536, 1048576, 4194304};
static const int numMsgSizes = sizeof(msgSizes) / sizeof(msgSizes[0]);

// Rounds per size. One is enough for the mechanism check; the repetition is
// for the race.
#define ROUNDS_PER_SIZE 8

#define GUARD_BYTES 64
#define GUARD_FILL 0xa5

// How long PE 1 keeps re-reading a buffer that failed, to tell an ack that
// merely arrived early from data that never arrived at all.
#define SETTLE_SECONDS 2.0

CpvDeclare(int, sizeIdx);
CpvDeclare(int, round);
CpvDeclare(char *, bufBase);
CpvDeclare(size_t, bufLen);
CpvDeclare(CmiNcpyBuffer, localBuf);
CpvDeclare(CmiNcpyBuffer, remoteBuf);
CpvDeclare(long, earlyAcks); // PE 0: acks the runtime let onto the pool

CpvDeclare(int, prepareHIdx);
CpvDeclare(int, bufReadyHIdx);
CpvDeclare(int, verifyHIdx);
CpvDeclare(int, roundDoneHIdx);
CpvDeclare(int, exitHIdx);

static inline int otherPe() { return 1 - CmiMyPe(); }

struct RoundMsg {
  CmiMessageHeader header;
  size_t len;
  unsigned int seed;
};

struct BufMsg {
  CmiMessageHeader header;
  CmiNcpyBuffer buf;
};

struct PlainMsg {
  CmiMessageHeader header;
};

template <typename T> static T *newMsg(int handlerIdx) {
  T *msg = (T *)CmiAlloc(sizeof(T));
  CmiSetHandler(msg, handlerIdx);
  return msg;
}

// ------------------------------------------------------------- patterns ---

// A distinct fill per round, so a round that reads the previous round's bytes
// fails rather than passing on them.
static unsigned int roundSeed() {
  return 101u + 7u * (unsigned int)CpvAccess(round) +
         131u * (unsigned int)CpvAccess(sizeIdx);
}

static unsigned char patternByte(size_t i, unsigned int seed) {
  return (unsigned char)((i * 131u + seed * 17u + 7u) & 0xffu);
}

static void fillPattern(char *buf, size_t n, unsigned int seed) {
  for (size_t i = 0; i < n; ++i)
    buf[i] = (char)patternByte(i, seed);
}

// Index of the first byte that is not the pattern for seed, or n if the whole
// region matches.
static size_t firstMismatch(const volatile char *buf, size_t n,
                            unsigned int seed) {
  for (size_t i = 0; i < n; ++i) {
    if ((unsigned char)buf[i] != patternByte(i, seed))
      return i;
  }
  return n;
}

static size_t countMismatches(const volatile char *buf, size_t n,
                              unsigned int seed) {
  size_t bad = 0;
  for (size_t i = 0; i < n; ++i) {
    if ((unsigned char)buf[i] != patternByte(i, seed))
      ++bad;
  }
  return bad;
}

// ------------------------------------------------------ buffer lifecycle ---

static void allocBuf(size_t len, unsigned int seed) {
  CmiAssert(CpvAccess(bufBase) == nullptr);
  char *base = (char *)CmiAlloc(len + GUARD_BYTES);
  fillPattern(base, len, seed);
  memset(base + len, GUARD_FILL, GUARD_BYTES);

  CpvAccess(bufBase) = base;
  CpvAccess(bufLen) = len;
  CpvAccess(localBuf) = CmiNcpyBuffer(base, len);
}

static void freeBuf() {
  CpvAccess(localBuf).deregisterMem();
  CmiFree(CpvAccess(bufBase));
  CpvAccess(bufBase) = nullptr;
  CpvAccess(bufLen) = 0;
}

// --------------------------------------------------------------- the run ---

static void startRound();

// PE 0, on local completion of the put. This runs inside the runtime's
// CommRputLocalHandler, which is where the ordering scope is taken, so the
// send below is the very message the fix is about.
static void rdmaAckHandler(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;
  if (CmiMyPe() != 0)
    CmiAbort("PE %d: a put this PE did not initiate raised an ack\n",
             CmiMyPe());
  if (info->ackMode != CMK_SRC_DEST_ACK)
    CmiAbort("PE 0: ackMode %d, expected CMK_SRC_DEST_ACK (%d)\n",
             (int)info->ackMode, (int)CMK_SRC_DEST_ACK);

  RoundMsg *msg = newMsg<RoundMsg>(CpvAccess(verifyHIdx));
  msg->len = CpvAccess(bufLen);
  msg->seed = roundSeed();

  // Only the one-sided path has anything to order: the copy-based fallback
  // raises this ack from putDataHandler on the destination, by which point the
  // bytes are already there, so the pool is free to carry it.
  const long poolSendsBefore = CmiIpcMessagesSent();
  CmiSyncSendAndFree(otherPe(), sizeof(RoundMsg), msg);
  if (!CmiUseCopyBasedRDMA && CmiIpcMessagesSent() != poolSendsBefore)
    CpvAccess(earlyAcks)++;
}

// PE 0: ask PE 1 for a freshly poisoned buffer, or finish.
static void startRound() {
  CmiAssert(CmiMyPe() == 0);
  if (CpvAccess(sizeIdx) == numMsgSizes) {
    CmiSyncBroadcastAllAndFree(sizeof(PlainMsg),
                               newMsg<PlainMsg>(CpvAccess(exitHIdx)));
    return;
  }

  RoundMsg *msg = newMsg<RoundMsg>(CpvAccess(prepareHIdx));
  msg->len = msgSizes[CpvAccess(sizeIdx)];
  msg->seed = roundSeed();
  CmiSyncSendAndFree(otherPe(), sizeof(RoundMsg), msg);
}

// PE 1: poison a destination buffer for this round and hand it to PE 0. The
// poison is the pattern of a seed no round uses, so a buffer the put has not
// reached yet cannot pass the check by accident.
static void prepareHandler(void *vmsg) {
  RoundMsg *msg = (RoundMsg *)vmsg;
  size_t len = msg->len;
  unsigned int seed = msg->seed;
  CmiFree(msg);

  allocBuf(len, ~seed);

  BufMsg *reply = newMsg<BufMsg>(CpvAccess(bufReadyHIdx));
  reply->buf = CpvAccess(localBuf);
  CmiSyncSendAndFree(otherPe(), sizeof(BufMsg), reply);
}

// PE 0: put this round's pattern into PE 1's buffer.
static void bufReadyHandler(void *vmsg) {
  BufMsg *msg = (BufMsg *)vmsg;
  CpvAccess(remoteBuf) = msg->buf;
  CmiFree(msg);

  allocBuf(CpvAccess(remoteBuf).cnt, roundSeed());
  CpvAccess(localBuf).rdmaPut(CpvAccess(remoteBuf), 0, nullptr, nullptr);
}

// PE 1: the ack says the buffer is filled, so it has to be filled now.
static void verifyHandler(void *vmsg) {
  RoundMsg *msg = (RoundMsg *)vmsg;
  const size_t len = msg->len;
  const unsigned int seed = msg->seed;
  CmiFree(msg);

  if (len != CpvAccess(bufLen))
    CmiAbort("PE 1: ack says %zu bytes, this round registered %zu\n", len,
             CpvAccess(bufLen));

  // volatile: the settle loop below re-reads bytes this thread never
  // wrote, so the reads must not be hoisted out of it.
  const volatile char *buf = CpvAccess(bufBase);
  size_t bad = firstMismatch(buf, len, seed);
  if (bad != len) {
    // Tell an ack that merely arrived early from data that is wrong or never
    // coming: re-read until the buffer settles or the deadline passes.
    const size_t missing = countMismatches(buf, len, seed);
    const double deadline = CmiWallTimer() + SETTLE_SECONDS;
    while (CmiWallTimer() < deadline && firstMismatch(buf, len, seed) != len)
      ;
    if (firstMismatch(buf, len, seed) == len)
      CmiAbort("PE 1: the put's acknowledgement arrived before its data. Byte "
               "%zu of %zu was still the poison value when the ack was "
               "delivered (%zu bytes of the buffer had not landed), and the "
               "whole buffer was correct %.3fs later. The destination ack was "
               "not held behind the put.\n",
               bad, len, missing, SETTLE_SECONDS);
    CmiAbort("PE 1: %zu of %zu bytes wrong after a put, first at byte %zu, "
             "and still wrong %.1fs later: this is not an ordering problem, "
             "the data itself is bad.\n",
             countMismatches(buf, len, seed), len, bad, SETTLE_SECONDS);
  }

  for (size_t i = 0; i < GUARD_BYTES; ++i) {
    if ((unsigned char)buf[len + i] != GUARD_FILL)
      CmiAbort("PE 1: a put of %zu bytes overran its buffer: guard byte %zu is "
               "0x%02x\n",
               len, i, (unsigned char)buf[len + i]);
  }

  freeBuf();
  CmiSyncSendAndFree(otherPe(), sizeof(PlainMsg),
                     newMsg<PlainMsg>(CpvAccess(roundDoneHIdx)));
}

// PE 0: this round checked out; move on.
static void roundDoneHandler(void *vmsg) {
  CmiFree(vmsg);
  freeBuf();

  if (++CpvAccess(round) == ROUNDS_PER_SIZE) {
    CmiPrintf("%d rounds of rdmaPut at %zu bytes: acknowledgement never "
              "preceded the data\n",
              ROUNDS_PER_SIZE, msgSizes[CpvAccess(sizeIdx)]);
    CpvAccess(round) = 0;
    CpvAccess(sizeIdx) += 1;
  }
  startRound();
}

static void exitHandler(void *vmsg) {
  CmiFree(vmsg);
  if (CpvAccess(bufBase) != nullptr)
    freeBuf();
  if (CmiMyPe() == 0 && CpvAccess(earlyAcks) != 0)
    CmiAbort("PE 0: %ld of %d destination acknowledgements were handed to the "
             "shared-memory pool. A pool message does not wait for the network, "
             "so it can overtake the put it is acknowledging; the runtime is "
             "supposed to keep these on the backend.\n",
             CpvAccess(earlyAcks), numMsgSizes * ROUNDS_PER_SIZE);
  CsdExitScheduler();
}

// ------------------------------------------------------------------ main ---

void ackOrderInit(int argc, char **argv) {
  // Set by the ctest entries that pass +ipc, so a run where the pool quietly
  // failed to come up fails instead of passing without testing anything.
  const int expectIpc = CmiGetArgFlagDesc(
      argv, "+expect-ipc", "fail unless the shared-memory pool is up");

  CpvInitialize(int, sizeIdx);
  CpvInitialize(int, round);
  CpvInitialize(char *, bufBase);
  CpvInitialize(size_t, bufLen);
  CpvInitialize(CmiNcpyBuffer, localBuf);
  CpvInitialize(CmiNcpyBuffer, remoteBuf);
  CpvInitialize(long, earlyAcks);

  CpvInitialize(int, prepareHIdx);
  CpvAccess(prepareHIdx) = CmiRegisterHandler((CmiHandler)prepareHandler);
  CpvInitialize(int, bufReadyHIdx);
  CpvAccess(bufReadyHIdx) = CmiRegisterHandler((CmiHandler)bufReadyHandler);
  CpvInitialize(int, verifyHIdx);
  CpvAccess(verifyHIdx) = CmiRegisterHandler((CmiHandler)verifyHandler);
  CpvInitialize(int, roundDoneHIdx);
  CpvAccess(roundDoneHIdx) = CmiRegisterHandler((CmiHandler)roundDoneHandler);
  CpvInitialize(int, exitHIdx);
  CpvAccess(exitHIdx) = CmiRegisterHandler((CmiHandler)exitHandler);

  CmiSetDirectNcpyAckHandler(rdmaAckHandler);

  if (CmiNumPes() != 2 || CmiNumNodes() != 2)
    CmiAbort("This test needs exactly 2 PEs in 2 separate processes on one "
             "host. Run it as: <launcher> -n 2 ./reconverse_rdma_ipc_ack "
             "+pe 2 +ipc +expect-ipc\n");

  if (expectIpc && !CmiIpcEnabled())
    CmiAbort("PE %d: +expect-ipc, but the shared-memory pool is not up. "
             "Without it the destination ack has nothing to overtake the put "
             "with and this test proves nothing.\n",
             CmiMyPe());

  if (CmiMyPe() == 0) {
    CmiPrintf("rdmaPut ack ordering: %d sizes x %d rounds, %s, ipc=%s\n",
              numMsgSizes, ROUNDS_PER_SIZE,
              CmiUseCopyBasedRDMA ? "copy-based path" : "network RDMA path",
              CmiIpcImplName());
    if (CmiUseCopyBasedRDMA)
      CmiPrintf("note: no one-sided put on this backend, so the ack cannot "
                "race the data here; only the checks' own plumbing is "
                "exercised.\n");
    startRound();
  }
}

int main(int argc, char **argv) { ConverseInit(argc, argv, ackOrderInit); }
