
/***************************************************************
  Converse Ping-pong to test the message latency and bandwidth
  Modified from Milind's ping-pong

  Sameer Kumar 02/07/05
 ****************************************************************/

#include "conv-rdma.h"
#include <converse.h>
#include <stdlib.h>

CpvDeclare(int, nCycles);
CpvDeclare(int, minMsgSize);
CpvDeclare(int, maxMsgSize);
CpvDeclare(int, factor);
CpvDeclare(bool, warmUp);
CpvDeclare(int, msgSize);
CpvDeclare(int, cycleNum);
CpvDeclare(int, warmUpDoneHandler);
CpvDeclare(int, exitHandler);
CpvDeclare(int, node1start);
CpvDeclare(int, node0start);
CpvDeclare(int, doNext);
CpvDeclare(int, startOperationHandler);
CpvDeclare(int, finishHandler);
CpvDeclare(CmiNcpyBuffer, buff);
CpvDeclare(CmiNcpyBuffer, send);
CpvStaticDeclare(double, startTime);
CpvStaticDeclare(double, endTime);

#define USE_PERSISTENT 0

#if USE_PERSISTENT
PersistentHandle h;
#endif

void startWarmUp() {
  // Small pingpong message to ensure that setup is completed
  if (CmiMyPe() == 0) {
    char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer));
    *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(buff);
    CmiSetHandler(msg, CpvAccess(node1start));
    CmiSyncSendAndFree(1, CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer), msg);
  }
}

// We finished for all message sizes. Exit now
CmiHandler exitHandlerFunc(char *msg) {
  // CmiFree(msg);
  CmiExit(0);
  return 0;
}

// the pingpong has finished, record message time
void ringFinished(char *msg) {
  CpvAccess(cycleNum) = 0;
  CpvAccess(warmUp) = true;
  if (CmiMyPe() == 1) {
    CpvAccess(endTime) = CmiWallTimer();
    size_t msgSizeDiff = CpvAccess(msgSize) - CmiMsgHeaderSizeBytes;

    // Print the time for that message size
    CmiPrintf("Size=%zu bytes, time=%lf microseconds one-way\n", msgSizeDiff,
              (1e6 * (CpvAccess(endTime) - CpvAccess(startTime))) /
                  (2. * CpvAccess(nCycles)));
  }

  // Have we finished all message sizes?
  if ((CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) < CpvAccess(maxMsgSize)) {
    // Increase message in powers of factor. Also add a converse header to that
    CpvAccess(msgSize) =
        (CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) * CpvAccess(factor) +
        CmiMsgHeaderSizeBytes;
    // CmiFree((void*)CpvAccess(buff).ptr);
    // start the ring again
    if (CmiMyPe() == 0) {
      char *content =
          (char *)(CmiAlloc(CpvAccess(msgSize) - CmiMsgHeaderSizeBytes));
      CpvAccess(buff) =
          CmiNcpyBuffer(content, CpvAccess(msgSize) - CmiMsgHeaderSizeBytes);
      startWarmUp();
    }
  } else {
    // exit
    exitHandlerFunc(NULL);
  }
}

void incomingHandlerFunc(void *msg) {
  // fprintf(stderr, "incomingHandlerFunc called on PE %d\n", CmiMyPe());
  NcpyOperationInfo info = *((NcpyOperationInfo *)(msg));
  if (CmiMyPe() == info.destPe) {
    if (CpvAccess(warmUp)) {
      CpvAccess(warmUp) = false;
      char *msg =
          (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer));
      *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(buff);
      CmiSetHandler(msg, CpvAccess(node0start));
      CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer), msg);
    } else {
      CpvAccess(cycleNum) += 1;
      if (CmiMyPe() == 0 && CpvAccess(cycleNum) == CpvAccess(nCycles)) {
        ringFinished(NULL);
      } else {
        char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
        CmiSetHandler(msg, CpvAccess(doNext));
        CmiSyncSendAndFree((CmiMyPe() + 1) % 2,
                           CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer), msg);
      }
    }
  }
}

void node1StartFunc(char *msg) {
  ringFinished(NULL);
  CpvAccess(warmUp) = true;
  char *content =
      (char *)(CmiAlloc(CpvAccess(msgSize) - CmiMsgHeaderSizeBytes));
  CpvAccess(buff) =
      CmiNcpyBuffer(content, CpvAccess(msgSize) - CmiMsgHeaderSizeBytes);
  CmiNcpyBuffer buffer = *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes));
  CpvAccess(send) = buffer;
  CpvAccess(startTime) = CmiWallTimer();
  CpvAccess(buff).rdmaGet(buffer, 0, NULL, NULL);
  // fprintf(stderr, "issue rdma get (size %d) on PE %d, cycle %d\n",
  // CpvAccess(msgSize), CmiMyPe(), CpvAccess(cycleNum));
}

void doNextFunc(char *msg) {
  CpvAccess(buff).rdmaGet(CpvAccess(send), sizeof(CmiNcpyBuffer),
                          (char *)(&CpvAccess(buff)), NULL);
}

void node0StartFunc(char *msg) {
  CmiNcpyBuffer buffer = *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes));
  CpvAccess(send) = buffer;
  CpvAccess(warmUp) = false;
  CpvAccess(buff).rdmaGet(buffer, sizeof(CmiNcpyBuffer),
                          (char *)(&CpvAccess(buff)), NULL);
  // fprintf(stderr, "issue rdma get (size %d) on PE %d, cycle %d\n",
  // CpvAccess(msgSize), CmiMyPe(), CpvAccess(cycleNum));
}

// Converse handler for beginning operation
CmiHandler startOperationHandlerFunc(char *msg) {
#if USE_PERSISTENT
  if (CmiMyPe() < CmiNumPes())
    h = CmiCreateCompressPersistent(otherPe, CpvAccess(maxMsgSize) + 1024, 200,
                                    CMI_FLOATING);
#endif
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
  CpvInitialize(CmiNcpyBuffer, buff);
  CpvInitialize(CmiNcpyBuffer, send);

  // Register Handlers
  CpvInitialize(int, exitHandler);
  CpvAccess(exitHandler) = CmiRegisterHandler((CmiHandler)exitHandlerFunc);
  CpvInitialize(int, startOperationHandler);
  CpvAccess(startOperationHandler) =
      CmiRegisterHandler((CmiHandler)startOperationHandlerFunc);
  CpvInitialize(int, node1start);
  CpvAccess(node1start) = CmiRegisterHandler((CmiHandler)node1StartFunc);
  CpvInitialize(int, node0start);
  CpvAccess(node0start) = CmiRegisterHandler((CmiHandler)node0StartFunc);
  CpvInitialize(int, doNext);
  CpvAccess(doNext) = CmiRegisterHandler((CmiHandler)doNextFunc);
  // set warmup run
  CpvAccess(warmUp) = true;
  CpvInitialize(int, finishHandler);
  CpvAccess(finishHandler) = CmiRegisterHandler((CmiHandler)ringFinished);

  CpvInitialize(double, startTime);
  CpvInitialize(double, endTime);

  int otherPe = CmiMyPe() ^ 1;

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

  if (CmiNumPes() != 2 && CmiMyPe() == 0) {
    CmiAbort(
        "This test is designed for only 2 pes and cannot be run on %d pe(s)!\n",
        CmiNumPes());
  }

  CpvAccess(msgSize) = CpvAccess(minMsgSize) + CmiMsgHeaderSizeBytes;
  if (CmiMyPe() == 1) {
    // Hack for now, as PE 1 will call ringFinished at the very beginning.
    CpvAccess(msgSize) =
        (CpvAccess(msgSize) - CmiMsgHeaderSizeBytes) / CpvAccess(factor) +
        CmiMsgHeaderSizeBytes;
  }

  CmiSetDirectNcpyAckHandler(incomingHandlerFunc);

  // Node 0 waits till all processors finish their topology processing
  if (CmiMyPe() == 0) {
    // Signal all PEs to begin computing
    char *content =
        (char *)(CmiAlloc(CpvAccess(minMsgSize) - CmiMsgHeaderSizeBytes));
    CpvAccess(buff) =
        CmiNcpyBuffer(content, CpvAccess(minMsgSize) - CmiMsgHeaderSizeBytes);
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
