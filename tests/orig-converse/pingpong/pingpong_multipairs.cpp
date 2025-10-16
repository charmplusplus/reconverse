
/**********************************************************
  Converse Ping-pong to test the message latency and bandwidth
  Modified from Milind's ping-pong

  Sameer Kumar 02/07/05
 ***************************************************/

#include <converse.h>
#include <stdlib.h>

enum { skip = 4 };

CpvDeclare(int, nCycles);
CpvDeclare(int, minMsgSize);
CpvDeclare(int, maxMsgSize);
CpvDeclare(int, factor);

CpvDeclare(int, recvNum);
CpvDeclare(double, sumTime);
CpvDeclare(int, msgSize);
CpvDeclare(int, cycleNum);
CpvDeclare(int, exitHandler);
CpvDeclare(int, reduceHandler);
CpvDeclare(int, startRingHandler);
CpvDeclare(int, node0Handler);
CpvDeclare(int, node1Handler);
CpvDeclare(int, startOperationHandler);
CpvStaticDeclare(double, startTime);
CpvStaticDeclare(double, endTime);

#define HALF CmiNumPes() / 2
#define USE_PERSISTENT 0

#if USE_PERSISTENT
PersistentHandle h;
#endif

// Start the pingpong for each message size
void startRing(char *msg_2) {
  CpvAccess(cycleNum) = -1;

  CmiFree(msg_2);

  int payloadSize;
  if (CpvAccess(msgSize) == 0) {
    payloadSize = CpvAccess(minMsgSize);
  } else {
    payloadSize =
        (CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) * CpvAccess(factor);
  }
  if (payloadSize > CpvAccess(maxMsgSize))
    payloadSize = CpvAccess(maxMsgSize);

  CpvAccess(msgSize) = payloadSize + CmiMsgHeaderSizeBytes;

  char *msg = (char *)CmiAlloc(CpvAccess(msgSize));
  *((int *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(msgSize);
  CmiSetHandler(msg, CpvAccess(node0Handler));
  CmiSyncSendAndFree(CmiMyPe(), CpvAccess(msgSize), msg);
}

void reduceHandlerFunc(char *msg) {
  CpvAccess(recvNum) += 1;
  CpvAccess(sumTime) += *((double *)(msg + (CmiMsgHeaderSizeBytes)));
  if (CpvAccess(recvNum) == HALF) {
    double us_time =
        (CpvAccess(sumTime)) / (2. * (CpvAccess(nCycles) - skip) * HALF) * 1e6;
    size_t msgSizeDiff = CpvAccess(msgSize) - CmiMsgHeaderSizeBytes;
    CmiPrintf("%zu\t\t  %.2lf   %.2f\n", msgSizeDiff, us_time,
              msgSizeDiff / us_time);
    CpvAccess(sumTime) = 0;
    CpvAccess(recvNum) = 0;

    if ((CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) < CpvAccess(maxMsgSize)) {
      for (int i = 0; i < HALF; i++) {
        void *sendmsg = CmiAlloc(CmiMsgHeaderSizeBytes);
        CmiSetHandler(sendmsg, CpvAccess(startRingHandler));
        CmiSyncSendAndFree(i, CmiMsgHeaderSizeBytes, sendmsg);
      }
    } else {
      // exit
      void *sendmsg = CmiAlloc(CmiMsgHeaderSizeBytes);
      CmiSetHandler(sendmsg, CpvAccess(exitHandler));
      CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, sendmsg);
    }
  }
  CmiFree(msg);
}
// the pingpong has finished, record message time
void ringFinished(char *msg) {
  CmiFree(msg);

  double elaps_time = CpvAccess(endTime) - CpvAccess(startTime);

#if 0
  // Print the time for that message size
  CmiPrintf("\t\t  %.2lf\n",
    (1e6*(CpvAccess(endTime)-CpvAccess(startTime)))/(2.*CpvAccess(nCycles)));
  //Have we finished all message sizes?
#endif
  int mysize = CmiMsgHeaderSizeBytes + sizeof(double);
  void *sendmsg = CmiAlloc(mysize);
  *((double *)((char *)sendmsg + CmiMsgHeaderSizeBytes)) = elaps_time;
  CmiSetHandler(sendmsg, CpvAccess(reduceHandler));
  CmiSyncSendAndFree(0, mysize, sendmsg);
}

// We finished for all message sizes. Exit now
CmiHandler exitHandlerFunc(char *msg) {
  CmiFree(msg);
  CsdExitScheduler();
  return 0;
}

// Handler on Node 0
CmiHandler node0HandlerFunc(char *msg) {
  CpvAccess(cycleNum)++;
  if (CpvAccess(cycleNum) == skip)
    CpvAccess(startTime) = CmiWallTimer();

  if (CpvAccess(cycleNum) == CpvAccess(nCycles)) {
    CpvAccess(endTime) = CmiWallTimer();
    ringFinished(msg);
  } else {
    CmiSetHandler(msg, CpvAccess(node1Handler));
    *((int *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(msgSize);

#if USE_PERSISTENT
    CmiUsePersistentHandle(&h, 1);
#endif
    CmiSyncSendAndFree(CmiMyPe() + HALF, CpvAccess(msgSize), msg);
#if USE_PERSISTENT
    CmiUsePersistentHandle(NULL, 0);
#endif
  }
  return 0;
}

CmiHandler node1HandlerFunc(char *msg) {
  CpvAccess(msgSize) = *((int *)(msg + CmiMsgHeaderSizeBytes));
  CmiSetHandler(msg, CpvAccess(node0Handler));

#if USE_PERSISTENT
  CmiUsePersistentHandle(&h, 1);
#endif
  CmiSyncSendAndFree(CmiMyPe() - HALF, CpvAccess(msgSize), msg);
#if USE_PERSISTENT
  CmiUsePersistentHandle(NULL, 0);
#endif
  return 0;
}

// Converse handler for beginning operation
CmiHandler startOperationHandlerFunc(char *msg) {
#if USE_PERSISTENT
  h = CmiCreatePersistent(otherPe, CpvAccess(maxMsgSize) + CmiMsgHeaderSizeBytes + 1024);
#endif
  if (CmiMyPe() == 0) {
    CmiPrintf("Multiple pair send/recv\n bytes \t\t latency(us)\t "
              "bandwidth(MBytes/sec)\n");
  }
  if (CmiMyPe() < CmiNumPes() / 2) {
    void *sendmsg = CmiAlloc(CmiMsgHeaderSizeBytes);
    CmiSetHandler(sendmsg, CpvAccess(startRingHandler));
    CmiSyncSendAndFree(CmiMyPe(), CmiMsgHeaderSizeBytes, sendmsg);
  }
  return 0;
}

// Converse main. Initialize variables and register handlers
CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, msgSize);
  CpvInitialize(int, recvNum);
  CpvInitialize(double, sumTime);
  CpvInitialize(int, cycleNum);
  CpvAccess(recvNum) = 0;
  CpvAccess(sumTime) = 0;
  CpvAccess(msgSize) = 0;

  CpvInitialize(int, nCycles);
  CpvInitialize(int, minMsgSize);
  CpvInitialize(int, maxMsgSize);
  CpvInitialize(int, factor);

  CpvAccess(nCycles) = 4004;
  CpvAccess(minMsgSize) = 4;
  CpvAccess(maxMsgSize) = 1 << 22;
  CpvAccess(factor) = 2;

  CpvInitialize(int, reduceHandler);
  CpvAccess(reduceHandler) = CmiRegisterHandler((CmiHandler)reduceHandlerFunc);
  CpvInitialize(int, exitHandler);
  CpvAccess(exitHandler) = CmiRegisterHandler((CmiHandler)exitHandlerFunc);
  CpvInitialize(int, startRingHandler);
  CpvAccess(startRingHandler) = CmiRegisterHandler((CmiHandler)startRing);
  CpvInitialize(int, node0Handler);
  CpvAccess(node0Handler) = CmiRegisterHandler((CmiHandler)node0HandlerFunc);
  CpvInitialize(int, node1Handler);
  CpvAccess(node1Handler) = CmiRegisterHandler((CmiHandler)node1HandlerFunc);
  CpvInitialize(int, startOperationHandler);
  CpvAccess(startOperationHandler) =
      CmiRegisterHandler((CmiHandler)startOperationHandlerFunc);

  CpvInitialize(double, startTime);
  CpvInitialize(double, endTime);

  int otherPe = CmiMyPe() ^ 1;

  // Set runtime cpuaffinity
  CmiInitCPUAffinity(argv);

  // Initialize CPU topology
  CmiInitCPUTopology(argv);

  // Wait for all PEs of the node to complete topology init
  CmiNodeAllBarrier();

  argc = CmiGetArgc(argv);
  if (argc >= 5) {
    CpvAccess(nCycles) = atoi(argv[1]);
    CpvAccess(minMsgSize) = atoi(argv[2]);
    CpvAccess(maxMsgSize) = atoi(argv[3]);
    CpvAccess(factor) = atoi(argv[4]);
  } else if (argc != 1) {
    if (CmiMyPe() == 0)
      CmiAbort("Usage: ./pingpong_multipairs <ncycles> <minsize> <maxsize> <increase factor>\nExample: ./pingpong_multipairs 4004 4 4194304 2\n");
    else
      CmiAbort("Invalid arguments provided to pingpong_multipairs");
  }

  if (CpvAccess(minMsgSize) <= 0 || CpvAccess(maxMsgSize) <= 0) {
    if (CmiMyPe() == 0)
      CmiAbort("Message sizes must be positive");
    else
      CmiAbort("Invalid message size configuration");
  }
  if (CpvAccess(minMsgSize) > CpvAccess(maxMsgSize)) {
    if (CmiMyPe() == 0)
      CmiAbort("minMsgSize (%d) must not exceed maxMsgSize (%d)\n",
               CpvAccess(minMsgSize), CpvAccess(maxMsgSize));
    else
      CmiAbort("Invalid message size range");
  }
  if (CpvAccess(nCycles) <= skip) {
    if (CmiMyPe() == 0)
      CmiAbort("nCycles (%d) must be greater than skip (%d)\n",
               CpvAccess(nCycles), skip);
    else
      CmiAbort("Invalid iteration count");
  }
  if (CpvAccess(factor) < 2) {
    if (CmiMyPe() == 0)
      CmiAbort("increase factor must be >= 2 (received %d)\n",
               CpvAccess(factor));
    else
      CmiAbort("Invalid increase factor");
  }

  if (CmiMyPe() == 0) {
    CmiPrintf("Multiple pair send/recv with iterations = %d, minMsgSize = %d, maxMsgSize = %d, increase factor = %d\n",
              CpvAccess(nCycles), CpvAccess(minMsgSize),
              CpvAccess(maxMsgSize), CpvAccess(factor));
  }

#if CMK_CONVERSE_MPI && CMK_SMP
  if (CmiMyPe() == 0 && CmiNumPhysicalNodes() == 1 &&
      CmiNumNodes() * 2 > CmiNumCores()) {
    CmiPrintf("Skipping pingpong_multipairs due to oversubscription.\n");

    char *exitMsg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
    CmiSetHandler((char *)exitMsg, CpvAccess(exitHandler));
    CmiSyncSend(0, CmiMsgHeaderSizeBytes, exitMsg);
    CmiSyncBroadcastAndFree(CmiMsgHeaderSizeBytes, exitMsg);

    return 0;
  }
#endif

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
