/* RDMA ping: a correctness test for the Converse zerocopy Direct API.
 *
 * rdma_pingpong measures how long an rdmaGet takes; nothing there checks that
 * the bytes which arrive are the bytes that were sent. This test does that,
 * for both directions of the Direct API, over a sweep of sizes:
 *
 *   PE 1 fills a registered buffer with a known pattern.
 *   PE 0 rdmaGets it and verifies every byte      -> exercises CmiIssueRget.
 *   PE 0 refills its buffer with a second pattern and rdmaPuts it back.
 *   PE 1 verifies every byte                      -> exercises CmiIssueRput.
 *
 * The two PEs must live in different processes. CmiIssueRget and CmiIssueRput
 * short-circuit to a memcpy when source and destination are on the same node,
 * which would leave the network path untested.
 */
#include "conv-rdma.h"
#include <converse.h>
#include <stdio.h>
#include <string.h>

// Sizes swept, in bytes. The odd and unaligned values are deliberate: they
// catch off-by-one and alignment assumptions that powers of two hide.
static const size_t msgSizes[] = {1, 7, 63, 1024, 4095, 65536};
static const int numMsgSizes = sizeof(msgSizes) / sizeof(msgSizes[0]);

// Bytes kept past the end of the registered region and touched by nothing, so
// a transfer that overruns its buffer lands in them instead of in unrelated
// memory.
#define GUARD_BYTES 64
#define GUARD_FILL 0xa5

// Three distinct fills, so a transfer that silently moves nothing fails the
// check instead of passing on whatever the buffer already held.
#define TARGET_SEED 11u // PE 1's buffer, before the get
#define POISON_SEED 47u // PE 0's buffer, before the get
#define SOURCE_SEED 83u // PE 0's buffer, before the put

enum Phase { PHASE_IDLE, PHASE_GET, PHASE_PUT };

CpvDeclare(int, sizeIdx);    // index into msgSizes, driven by PE 0
CpvDeclare(int, phase);      // which transfer PE 0 is waiting on
CpvDeclare(int, putSignals); // PE 1: completion signals seen for this put
CpvDeclare(char *, bufBase); // allocation backing localBuf, plus its guard
CpvDeclare(size_t, bufLen);  // registered length, excluding the guard
CpvDeclare(CmiNcpyBuffer, localBuf);
CpvDeclare(CmiNcpyBuffer, remoteBuf);

CpvDeclare(int, prepareHIdx);
CpvDeclare(int, bufReadyHIdx);
CpvDeclare(int, getDoneHIdx);
CpvDeclare(int, putDoneHIdx);
CpvDeclare(int, sizeDoneHIdx);
CpvDeclare(int, exitHIdx);

// This test runs on exactly two PEs, so "the other PE" is the one this is not.
static inline int otherPe() { return 1 - CmiMyPe(); }

struct SizeMsg {
  CmiMessageHeader header;
  size_t len;
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

static void sendPlain(int destPe, int handlerIdx) {
  CmiSyncSendAndFree(destPe, sizeof(PlainMsg), newMsg<PlainMsg>(handlerIdx));
}

// ------------------------------------------------------------- patterns ---

static unsigned char patternByte(size_t i, unsigned int seed) {
  return (unsigned char)((i * 131u + seed * 17u + 7u) & 0xffu);
}

static void fillPattern(char *buf, size_t n, unsigned int seed) {
  for (size_t i = 0; i < n; ++i)
    buf[i] = (char)patternByte(i, seed);
}

// Abort unless buf holds exactly the pattern for seed and its guard region is
// untouched. what names the transfer being checked, for the failure message.
static void checkRegion(const char *what, const char *buf, size_t n,
                        unsigned int seed) {
  for (size_t i = 0; i < n; ++i) {
    if ((unsigned char)buf[i] != patternByte(i, seed))
      CmiAbort("PE %d: %s of %zu bytes: byte %zu is 0x%02x, expected 0x%02x\n",
               CmiMyPe(), what, n, i, (unsigned char)buf[i],
               patternByte(i, seed));
  }
  for (size_t i = 0; i < GUARD_BYTES; ++i) {
    if ((unsigned char)buf[n + i] != GUARD_FILL)
      CmiAbort("PE %d: %s of %zu bytes: guard byte %zu past the end of the "
               "buffer was overwritten with 0x%02x\n",
               CmiMyPe(), what, n, i, (unsigned char)buf[n + i]);
  }
}

// ------------------------------------------------------ buffer lifecycle ---

static void allocBuf(size_t len, unsigned int seed) {
  CmiAssert(CpvAccess(bufBase) == nullptr);
  char *base = (char *)CmiAlloc(len + GUARD_BYTES);
  fillPattern(base, len, seed);
  memset(base + len, GUARD_FILL, GUARD_BYTES);

  CpvAccess(bufBase) = base;
  CpvAccess(bufLen) = len;
  // Only the first len bytes are registered, so the guard stays outside the
  // memory region the peer's NIC is allowed to reach.
  CpvAccess(localBuf) = CmiNcpyBuffer(base, len);
}

static void freeBuf() {
  CpvAccess(localBuf).deregisterMem();
  CmiFree(CpvAccess(bufBase));
  CpvAccess(bufBase) = nullptr;
  CpvAccess(bufLen) = 0;
}

// --------------------------------------------------------------- the run ---

static void startSize();     // PE 0
static void notePutSignal(); // PE 1

// Completion callback for every Direct API operation in this process. It runs
// on the communication progress path, so it posts a Converse message rather
// than issuing the next transfer itself.
static void rdmaAckHandler(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;

  if (CmiMyPe() != 0) {
    // PE 1 only cares about acks saying data landed here. While serving PE 0's
    // get, the copy-based path also raises a source ack on this PE.
    if (info->destPe == CmiMyPe())
      notePutSignal();
    return;
  }

  if (CpvAccess(phase) == PHASE_GET) {
    // Local completion of a get means the bytes are in localBuf.
    sendPlain(CmiMyPe(), CpvAccess(getDoneHIdx));
  } else {
    CmiAssert(CpvAccess(phase) == PHASE_PUT);
    sendPlain(otherPe(), CpvAccess(putDoneHIdx));
  }
}

// PE 0: ask PE 1 for a buffer of the next size, or finish.
static void startSize() {
  CmiAssert(CmiMyPe() == 0);
  if (CpvAccess(sizeIdx) == numMsgSizes) {
    CmiSyncBroadcastAllAndFree(sizeof(PlainMsg),
                               newMsg<PlainMsg>(CpvAccess(exitHIdx)));
    return;
  }

  SizeMsg *msg = newMsg<SizeMsg>(CpvAccess(prepareHIdx));
  msg->len = msgSizes[CpvAccess(sizeIdx)];
  CmiSyncSendAndFree(otherPe(), sizeof(SizeMsg), msg);
}

// PE 1: register a buffer for this size and hand its descriptor to PE 0.
static void prepareHandler(void *vmsg) {
  SizeMsg *msg = (SizeMsg *)vmsg;
  size_t len = msg->len;
  CmiFree(msg);

  allocBuf(len, TARGET_SEED);
  CpvAccess(putSignals) = 0;

  BufMsg *reply = newMsg<BufMsg>(CpvAccess(bufReadyHIdx));
  reply->buf = CpvAccess(localBuf);
  CmiSyncSendAndFree(otherPe(), sizeof(BufMsg), reply);
}

// PE 0: PE 1's buffer is registered and filled, so pull it across.
static void bufReadyHandler(void *vmsg) {
  BufMsg *msg = (BufMsg *)vmsg;
  CpvAccess(remoteBuf) = msg->buf;
  CmiFree(msg);

  allocBuf(CpvAccess(remoteBuf).cnt, POISON_SEED);
  CpvAccess(phase) = PHASE_GET;
  CpvAccess(localBuf).rdmaGet(CpvAccess(remoteBuf), 0, nullptr, nullptr);
}

// PE 0: the get landed. Check it, then send a different pattern back, so the
// put cannot pass by leaving the bytes the get just moved.
static void getDoneHandler(void *vmsg) {
  CmiFree(vmsg);
  checkRegion("data received by rdmaGet", CpvAccess(bufBase), CpvAccess(bufLen),
              TARGET_SEED);

  fillPattern(CpvAccess(bufBase), CpvAccess(bufLen), SOURCE_SEED);
  CpvAccess(phase) = PHASE_PUT;
  CpvAccess(localBuf).rdmaPut(CpvAccess(remoteBuf), 0, nullptr, nullptr);
}

// PE 1: a true RDMA put raises no completion on the target, so PE 0's message
// is the only signal; the copy-based path delivers the payload as an active
// message and raises a destination ack here as well. Requiring every signal
// the current mode produces makes the check independent of their order.
static void notePutSignal() {
  CmiAssert(CmiMyPe() == 1);
  const int expected = CmiUseCopyBasedRDMA ? 2 : 1;
  if (++CpvAccess(putSignals) != expected)
    return;

  checkRegion("data received by rdmaPut", CpvAccess(bufBase), CpvAccess(bufLen),
              SOURCE_SEED);
  freeBuf();
  sendPlain(otherPe(), CpvAccess(sizeDoneHIdx));
}

static void putDoneHandler(void *vmsg) {
  CmiFree(vmsg);
  notePutSignal();
}

// PE 0: both transfers for this size checked out; move on.
static void sizeDoneHandler(void *vmsg) {
  CmiFree(vmsg);
  CmiPrintf("Size=%zu bytes: rdmaGet and rdmaPut verified\n",
            CpvAccess(bufLen));

  freeBuf();
  CpvAccess(phase) = PHASE_IDLE;
  CpvAccess(sizeIdx) += 1;
  startSize();
}

static void exitHandler(void *vmsg) {
  CmiFree(vmsg);
  if (CpvAccess(bufBase) != nullptr)
    freeBuf();
  CsdExitScheduler();
}

// ------------------------------------------------------------------ main ---

void rdmaPingInit(int argc, char **argv) {
  CpvInitialize(int, sizeIdx);
  CpvInitialize(int, phase);
  CpvInitialize(int, putSignals);
  CpvInitialize(char *, bufBase);
  CpvInitialize(size_t, bufLen);
  CpvInitialize(CmiNcpyBuffer, localBuf);
  CpvInitialize(CmiNcpyBuffer, remoteBuf);

  CpvInitialize(int, prepareHIdx);
  CpvAccess(prepareHIdx) = CmiRegisterHandler((CmiHandler)prepareHandler);
  CpvInitialize(int, bufReadyHIdx);
  CpvAccess(bufReadyHIdx) = CmiRegisterHandler((CmiHandler)bufReadyHandler);
  CpvInitialize(int, getDoneHIdx);
  CpvAccess(getDoneHIdx) = CmiRegisterHandler((CmiHandler)getDoneHandler);
  CpvInitialize(int, putDoneHIdx);
  CpvAccess(putDoneHIdx) = CmiRegisterHandler((CmiHandler)putDoneHandler);
  CpvInitialize(int, sizeDoneHIdx);
  CpvAccess(sizeDoneHIdx) = CmiRegisterHandler((CmiHandler)sizeDoneHandler);
  CpvInitialize(int, exitHIdx);
  CpvAccess(exitHIdx) = CmiRegisterHandler((CmiHandler)exitHandler);

  CmiSetDirectNcpyAckHandler(rdmaAckHandler);

  // Exactly two PEs, one per process. Two PEs in the same process would make
  // CmiIssueRget and CmiIssueRput take their intra-node shortcut, a plain
  // memcpy, which is not the path this test exists to check. Every PE runs the
  // check, so an unusable configuration tears the whole job down.
  if (CmiNumPes() != 2 || CmiNumNodes() != 2) {
    CmiAbort("This test needs exactly 2 PEs in 2 separate processes, so that "
             "the transfers go over the network RDMA path. Run it as: "
             "<launcher> -n 2 ./reconverse_rdma_ping +pe 2\n");
  }

  if (CmiMyPe() == 0) {
    CmiPrintf("RDMA ping: verifying rdmaGet and rdmaPut over %d sizes, %s\n",
              numMsgSizes,
              CmiUseCopyBasedRDMA ? "copy-based path" : "network RDMA path");
    startSize();
  }
}

int main(int argc, char **argv) { ConverseInit(argc, argv, rdmaPingInit); }
