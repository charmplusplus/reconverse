#include "conv-rdma.h"
#include <converse.h>
#include <stdlib.h>

CpvDeclare(int, nCycles);
CpvDeclare(int, minMsgSize);
CpvDeclare(int, maxMsgSize);
CpvDeclare(int, factor);
CpvDeclare(bool, warmUp);
CpvDeclare(int, msgSize);
CpvDeclare(int, currentCycle);

CpvDeclare(CmiNcpyBuffer, localBuf);
CpvDeclare(CmiNcpyBuffer, remoteBuf);
CpvStaticDeclare(double, startTime);
CpvStaticDeclare(double, endTime);
CpvDeclare(int, setRemoteBufHIdx);
CpvDeclare(int, rmaGetSignalHIdx);
CpvDeclare(int, exitBenchmarkHIdx);

void setupRMABuf();

void startRing() {
  CmiAssert(CmiMyPe() == 0);
  setupRMABuf();
}

void setupRMABuf() {
  // If there is a previous buffer, clean it up
  if (CpvAccess(localBuf).ptr != nullptr) {
    CpvAccess(localBuf).deregisterMem();
    CmiFree((void *)CpvAccess(localBuf).ptr);
  }
  // setup a new localBuf
  char *content = (char *)(CmiAlloc(CpvAccess(msgSize)));
  CpvAccess(localBuf) = CmiNcpyBuffer(content, CpvAccess(msgSize));
  // send the localBuf back
  char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer));
  *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(localBuf);
  CmiSetHandler(msg, CpvAccess(setRemoteBufHIdx));
  CmiSyncSendAndFree(1 - CmiMyPe(),
                     CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer), msg);
}

void startWarmUp();

void setRemoteBufHandler(char *msg) {
  CpvAccess(remoteBuf) = *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes));
  if (CmiMyPe() == 1) {
    CpvAccess(msgSize) = CpvAccess(remoteBuf).cnt;
    setupRMABuf();
  } else {
    startWarmUp();
  }
}

void doRMA();

void startWarmUp() {
  CmiAssert(CmiMyPe() == 0);
  CpvAccess(warmUp) = true;
  doRMA();
}

void doRMA() {
  CpvAccess(localBuf).rdmaGet(CpvAccess(remoteBuf), 0, nullptr, nullptr);
}

void rmaGetLocalComp(void *context) {
  // fprintf(stderr, "rmaGetLocalComp called on PE %d\n", CmiMyPe());
  NcpyOperationInfo info = *((NcpyOperationInfo *)(context));
  if (CmiMyPe() == info.srcPe) {
    // We assume no remote completion notification
    return;
  }
  // send remote completion signal
  char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(msg, CpvAccess(rmaGetSignalHIdx));
  CmiSyncSendAndFree(1 - CmiMyPe(), CmiMsgHeaderSizeBytes, msg);
}

void endRing();

void rmaGetSignalHandler(char *) {
  if (CmiMyPe() == 1) {
    // PE 1 just do a "pong" get back
    doRMA();
    return;
  }
  // PE 0
  if (CpvAccess(warmUp)) {
    CpvAccess(warmUp) = false;
    // record start time
    CpvAccess(startTime) = CmiWallTimer();
    CpvAccess(currentCycle) = 0;
    doRMA();
  } else {
    CpvAccess(currentCycle) += 1;
    if (CpvAccess(currentCycle) < CpvAccess(nCycles)) {
      doRMA();
    } else {
      endRing();
    }
  }
}

void exitBenchmark();

void endRing() {
  CmiAssert(CmiMyPe() == 0);
  CpvAccess(endTime) = CmiWallTimer();

  // Print the time for that message size
  CmiPrintf("Size=%zu bytes, time=%lf microseconds one-way\n",
            CpvAccess(msgSize),
            (1e6 * (CpvAccess(endTime) - CpvAccess(startTime))) /
                (2. * CpvAccess(nCycles)));

  // Have we finished all message sizes?
  if (CpvAccess(msgSize) < CpvAccess(maxMsgSize)) {
    // Increase message in powers of factor. Also add a converse header to that
    CpvAccess(msgSize) = CpvAccess(msgSize) * CpvAccess(factor);
    startRing();
  } else {
    exitBenchmark();
  }
}
void exitBenchmarkHandler(char *);

void exitBenchmark() {
  char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(msg, CpvAccess(exitBenchmarkHIdx));
  CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, msg);
}

void exitBenchmarkHandler(char *) {
  // clean up resources
  if (CpvAccess(localBuf).ptr != nullptr) {
    CpvAccess(localBuf).deregisterMem();
    CmiFree((void *)CpvAccess(localBuf).ptr);
  }
  fprintf(stderr, "exitBenchmarkHandler called on PE %d\n", CmiMyPe());
  CsdExitScheduler();
}

// Converse main. Initialize variables and register handlers
CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, msgSize);
  CpvInitialize(int, currentCycle);

  CpvInitialize(int, nCycles);
  CpvInitialize(int, minMsgSize);
  CpvInitialize(int, maxMsgSize);
  CpvInitialize(int, factor);
  CpvInitialize(bool, warmUp);
  CpvInitialize(CmiNcpyBuffer, localBuf);
  CpvInitialize(CmiNcpyBuffer, remoteBuf);

  // Register Handlers
  CpvInitialize(int, setRemoteBufHIdx);
  CpvAccess(setRemoteBufHIdx) =
      CmiRegisterHandler((CmiHandler)setRemoteBufHandler);
  CpvInitialize(int, rmaGetSignalHIdx);
  CpvAccess(rmaGetSignalHIdx) =
      CmiRegisterHandler((CmiHandler)rmaGetSignalHandler);
  CpvInitialize(int, exitBenchmarkHIdx);
  CpvAccess(exitBenchmarkHIdx) =
      CmiRegisterHandler((CmiHandler)exitBenchmarkHandler);

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
  if (argc == 5) {
    CpvAccess(nCycles) = atoi(argv[1]);
    CpvAccess(minMsgSize) = atoi(argv[2]);
    CpvAccess(maxMsgSize) = atoi(argv[3]);
    CpvAccess(factor) = atoi(argv[4]);
  } else if (argc == 1) {
    // use default arguments
    CpvAccess(nCycles) = 100;
    CpvAccess(minMsgSize) = 1 << 9;
    CpvAccess(maxMsgSize) = 1 << 14;
    CpvAccess(factor) = 2;
  } else {
    if (CmiMyPe() == 0)
      CmiAbort("Usage: ./pingpong <ncycles> <minsize> <maxsize> <increase "
               "factor> \nExample: "
               "./pingpong 100 2 128 2\n");
  }

  if (CmiMyPe() == 0) {
    CmiPrintf("Pingpong with iterations = %d, minMsgSize = %d, maxMsgSize = "
              "%d, increase "
              "factor = %d\n",
              CpvAccess(nCycles), CpvAccess(minMsgSize), CpvAccess(maxMsgSize),
              CpvAccess(factor));
  }

  if (CmiNumNodes() != 2 && CmiNumPes() != 2 && CmiMyPe() == 0) {
    CmiAbort("This test is designed for only 2 nodes and with 1 PE per node\n");
  }

  CpvAccess(msgSize) = CpvAccess(minMsgSize);

  CmiSetDirectNcpyAckHandler(rmaGetLocalComp);

  // Node 0 waits till all processors finish their topology processing
  CmiNodeBarrier();

  if (CmiMyPe() == 0) {
    startRing();
  }
  return 0;
}

int main(int argc, char *argv[]) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
