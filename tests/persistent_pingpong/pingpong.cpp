/***************************************************************
  Converse Ping-pong over a persistent channel.

  This is tests/orig-converse/pingpong/pingpong.cpp with one change: the two
  timed sends go out through a PersistentHandle instead of the ordinary send
  path.  Everything else -- the handler structure, the warm-up, the timing
  points, the size sweep, the printf format -- is deliberately identical, so a
  difference against that benchmark is attributable to the channel and not to
  the harness around it.

  Why this is the "persistent + RDMA" configuration: a persistent channel
  allocates PERSIST_BUFFERS_NUM receive buffers on the destination once and,
  when the backend is RMA-capable, registers them so the sender writes the
  payload straight in with a one-sided put (see comm_backend::issueRput in
  src/persist-comm.cpp).  So the per-message cost of allocating a buffer,
  registering memory and matching the message is paid at setup rather than on
  every send, and the payload itself moves by RDMA.

  Note both PEs set up their own channel to the other, because each direction
  of the pingpong is a separate persistent channel.  The channel is sized for
  the largest message in the sweep so the same handle serves every size; the
  buffers are allocated once, at setup.

  Sends out of a received message are intentional.  A message that arrived on
  a persistent channel lives in one of that channel's receive buffers, and
  handing it to a send-and-free call is the documented way to release it (see
  include/persistent.h), so the pong path forwards the buffer it was given
  rather than allocating a fresh one -- exactly as the non-persistent pingpong
  does.

  Usage: ./pingpong <ncycles> <minsize> <maxsize> <increase factor>
 ****************************************************************/

#include <converse.h>
#include <stdlib.h>
#include <string.h>

CpvDeclare(int, nCycles);
CpvDeclare(int, minMsgSize);
CpvDeclare(int, maxMsgSize);
CpvDeclare(int, factor);
CpvDeclare(bool, warmUp);
CpvDeclare(int, msgSize);
CpvDeclare(int, cycleNum);
CpvDeclare(int, warmUpDoneHandler);
CpvDeclare(int, exitHandler);
CpvDeclare(int, node0Handler);
CpvDeclare(int, node1Handler);
CpvDeclare(int, startOperationHandler);
CpvStaticDeclare(double, startTime);
CpvStaticDeclare(double, endTime);

/* The channel from this PE to the other one. */
CpvStaticDeclare(PersistentHandle, toOther);

/* -freshbuf: allocate a new message for each reply instead of forwarding the
   one that was received.
   The default (forwarding) is what the non-persistent pingpong does and what
   the persistent API documents as the way to release a received buffer, but
   in a pingpong the buffer being forwarded belongs to the PEER's channel and
   goes back out on this PE's own channel.  That cross-channel reuse is the
   one thing this benchmark does that the repo's persistent test does not, and
   it faults when LCI's shared-memory transport is enabled.  This flag isolates
   it. */
CpvStaticDeclare(int, freshBuf);

/* Hand back the received buffer and return a fresh one carrying the same
   payload size, so the reply never leaves a peer-owned buffer. */
static char *replyBuffer(char *msg, int size) {
  if (!CpvAccess(freshBuf))
    return msg;
  char *fresh = (char *)CmiAlloc(size);
  memcpy(fresh + CmiMsgHeaderSizeBytes, msg + CmiMsgHeaderSizeBytes,
         sizeof(int));
  CmiFree(msg);
  return fresh;
}

/* Extra room beyond the largest payload, so one channel covers the whole
   sweep and the buffers are never resized mid-run. */
#define CHANNEL_SLACK 1024

/* Send on the persistent channel.  CmiUsePersistentHandle installs the handle
   for the sends that follow and has to be cancelled straight after, or
   unrelated traffic (the exit broadcast) would be routed onto the channel
   too. */
static void sendPersistent(int destPE, int size, void *msg) {
  CmiUsePersistentHandle(&CpvAccess(toOther), 1);
  CmiSyncSendAndFree(destPE, size, msg);
  CmiUsePersistentHandle(NULL, 0);
}

// Start the pingpong for each message size
void startRing() {
  CpvAccess(cycleNum) = 0;
  char *msg = (char *)CmiAlloc(CpvAccess(msgSize));
  *((int *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(msgSize);
  CmiSetHandler(msg, CpvAccess(node0Handler));
  CmiSyncSendAndFree(0, CpvAccess(msgSize), msg);
}

// the pingpong has finished, record message time
void ringFinished(char *msg) {
  size_t msgSizeDiff = CpvAccess(msgSize) - CmiMsgHeaderSizeBytes;
  CmiFree(msg);

  // Print the time for that message size
  CmiPrintf("Size=%zu bytes, time=%lf microseconds one-way\n", msgSizeDiff,
            (1e6 * (CpvAccess(endTime) - CpvAccess(startTime))) /
                (2. * CpvAccess(nCycles)));

  // Have we finished all message sizes?
  if ((CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) < CpvAccess(maxMsgSize)) {
    // Increase message in powers of factor. Also add a converse header to that
    CpvAccess(msgSize) =
        (CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) * CpvAccess(factor) +
        CmiMsgHeaderSizeBytes;
    // start the ring again
    startRing();
  } else {
    // exit.  Deliberately NOT on the persistent channel: it is a broadcast,
    // and the channel is point to point.
    void *sendmsg = CmiAlloc(CmiMsgHeaderSizeBytes);
    CmiSetHandler(sendmsg, CpvAccess(exitHandler));
    CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, sendmsg);
  }
}

void startWarmUp() {
  // Small pingpong message to ensure that setup is completed.  For this
  // benchmark the warm-up does double duty: it also gives the persistent
  // channel handshake a round trip to complete, so the first timed size is
  // not charged for channel setup.
  char *msg = (char *)CmiAlloc(CpvAccess(msgSize));
  *((int *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(msgSize);
  CmiSetHandler(msg, CpvAccess(node0Handler));
  CmiSyncSendAndFree(0, CpvAccess(msgSize), msg);
}

// Handler on Node 0 which starts pingpong on warmup completion
void warmUpDoneHandlerFunc(char *msg) {
  CmiFree(msg);
  // Warmup phase completed. Start pingpong
  startRing();
}

// We finished for all message sizes. Exit now
CmiHandler exitHandlerFunc(char *msg) {
  CmiFree(msg);
  CmiDestroyAllPersistent();
  CsdExitScheduler();
  return 0;
}

// Handler on Node 0
CmiHandler node0HandlerFunc(char *msg) {
  if (CpvAccess(warmUp))
    CpvAccess(warmUp) = false;
  else
    CpvAccess(cycleNum)++;

  // Begin timer for the first iteration
  if (CpvAccess(cycleNum) == 1)
    CpvAccess(startTime) = CmiWallTimer();

  // Stop timer for the last iteration
  if (CpvAccess(cycleNum) == CpvAccess(nCycles)) {
    CpvAccess(endTime) = CmiWallTimer();
    ringFinished(msg);
  } else {
    msg = (char *)replyBuffer(msg, CpvAccess(msgSize));
    CmiSetHandler(msg, CpvAccess(node1Handler));
    *((int *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(msgSize);
    sendPersistent(1, CpvAccess(msgSize), msg);
  }
  return 0;
}

CmiHandler node1HandlerFunc(char *msg) {
  CpvAccess(msgSize) = *((int *)(msg + CmiMsgHeaderSizeBytes));

  msg = (char *)replyBuffer(msg, CpvAccess(msgSize));
  if (CpvAccess(warmUp)) {
    CmiSetHandler(msg, CpvAccess(warmUpDoneHandler));
    CpvAccess(warmUp) = false;
  } else
    CmiSetHandler(msg, CpvAccess(node0Handler));

  sendPersistent(0, CpvAccess(msgSize), msg);
  return 0;
}

// Converse handler for beginning operation
CmiHandler startOperationHandlerFunc(char *msg) {
  // Each PE opens its own channel to the other; the two directions are
  // separate channels.  Sized for the largest message in the sweep so one
  // handle covers every size.
  int otherPe = CmiMyPe() ^ 1;
  CpvAccess(toOther) = CmiCreatePersistent(
      otherPe, CpvAccess(maxMsgSize) + CmiMsgHeaderSizeBytes + CHANNEL_SLACK);

  if (CmiMyPe() == 0)
    startWarmUp();
  return 0;
}

// Converse main. Initialize variables and register handlers
CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, msgSize);
  CpvInitialize(int, cycleNum);

  CpvInitialize(int, nCycles);
  CpvInitialize(int, minMsgSize);
  CpvInitialize(int, maxMsgSize);
  CpvInitialize(int, factor);
  CpvInitialize(bool, warmUp);
  CpvInitialize(PersistentHandle, toOther);
  CpvAccess(toOther) = NULL;
  CpvInitialize(int, freshBuf);
  CpvAccess(freshBuf) = CmiGetArgFlag(argv, "-freshbuf");

  // Register Handlers
  CpvInitialize(int, warmUpDoneHandler);
  CpvAccess(warmUpDoneHandler) =
      CmiRegisterHandler((CmiHandler)warmUpDoneHandlerFunc);
  CpvInitialize(int, exitHandler);
  CpvAccess(exitHandler) = CmiRegisterHandler((CmiHandler)exitHandlerFunc);
  CpvInitialize(int, node0Handler);
  CpvAccess(node0Handler) = CmiRegisterHandler((CmiHandler)node0HandlerFunc);
  CpvInitialize(int, node1Handler);
  CpvAccess(node1Handler) = CmiRegisterHandler((CmiHandler)node1HandlerFunc);
  CpvInitialize(int, startOperationHandler);
  CpvAccess(startOperationHandler) =
      CmiRegisterHandler((CmiHandler)startOperationHandlerFunc);

  // set warmup run
  CpvAccess(warmUp) = true;

  CpvInitialize(double, startTime);
  CpvInitialize(double, endTime);

  // Set runtime cpuaffinity
  CmiInitCPUAffinity(argv);

  // Initialize CPU topology
  CmiInitCPUTopology(argv);

  // Wait for all PEs of the node to complete topology init
  CmiNodeAllBarrier();

  // Update the argc after runtime parameters are extracted out
  argc = CmiGetArgc(argv);
  if (argc >= 5) {
    CpvAccess(nCycles) = atoi(argv[1]);
    CpvAccess(minMsgSize) = atoi(argv[2]);
    CpvAccess(maxMsgSize) = atoi(argv[3]);
    CpvAccess(factor) = atoi(argv[4]);
  } else if (argc == 1) {
    // use default arguments
    CpvAccess(nCycles) = 1000;
    CpvAccess(minMsgSize) = 1 << 9;
    CpvAccess(maxMsgSize) = 1 << 14;
    CpvAccess(factor) = 2;
  } else {
    if (CmiMyPe() == 0)
      CmiAbort("Usage: ./pingpong <ncycles> <minsize> <maxsize> <increase "
               "factor> \nExample: ./pingpong 100 2 128 2\n");
  }

  if (CmiMyPe() == 0) {
    CmiPrintf("Pingpong with iterations = %d, minMsgSize = %d, maxMsgSize = "
              "%d, increase factor = %d\n",
              CpvAccess(nCycles), CpvAccess(minMsgSize), CpvAccess(maxMsgSize),
              CpvAccess(factor));
  }

  if (CmiNumPes() != 2 && CmiMyPe() == 0) {
    CmiAbort(
        "This test is designed for only 2 pes and cannot be run on %d pe(s)!\n",
        CmiNumPes());
  }

  CpvAccess(msgSize) = CpvAccess(minMsgSize) + CmiMsgHeaderSizeBytes;

  // Node 0 waits till all processors finish their topology processing
  if (CmiMyPe() == 0) {
    // Signal all PEs to begin computing
    char *startOperationMsg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
    CmiSetHandler((char *)startOperationMsg, CpvAccess(startOperationHandler));
    CmiSyncBroadcastAndFree(CmiMsgHeaderSizeBytes, startOperationMsg);

    // start operation locally on PE 0
    startOperationHandlerFunc(NULL);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
