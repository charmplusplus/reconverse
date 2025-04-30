#include "converse.h"
#include <pthread.h>
#include <stdio.h>

CpvDeclare(int, test);
CpvDeclare(int, exitHandlerId);
CpvDeclare(int, nodeHandlerId);
CpvDeclare(int, beginIdleId);
CpvDeclare(int, stillIdleId);
CpvDeclare(int, longIdleId);

struct Message {
  CmiMessageHeader header;
};

void stop_handler(void *vmsg) { CsdExitScheduler(); }

void beginIdle(void *vmsg) {
  printf("BEGIN IDLE HANDLER CALLED on pe %d\n", CmiMyRank());
}

void stillIdle(void *vmsg) {
  printf("STILL IDLE HANDLER CALLED on pe %d\n", CmiMyRank());
}

void longIdle(void *vmsg) {
  printf("LONG IDLE HANDLER CALLED on pe %d\n", CmiMyRank());
  Message *msg = new Message;
  msg->header.handlerId = CpvAccess(exitHandlerId);
  msg->header.messageSize = sizeof(Message);
  CmiSyncSendAndFree(CmiMyRank(), msg->header.messageSize, msg);
}

void nodeQueueTest(void *msg) {
  printf("NODE QUEUE TEST on pe %d\n", CmiMyRank());
}

void ping_handler(void *vmsg) {
  printf("PING HANDLER CALLED\n");
  Message *msg = new Message;
  msg->header.handlerId = CpvAccess(nodeHandlerId);
  msg->header.messageSize = sizeof(Message);
  CmiSyncNodeSendAndFree(0, msg->header.messageSize, msg);
}

CmiStartFn mymain(int argc, char **argv) {
  CpvInitialize(int, test);
  CpvAccess(test) = 42;

  printf("My PE is %d\n", CmiMyRank());

  int handlerId = CmiRegisterHandler(ping_handler);

  if (CmiMyRank() == 0 && CmiMyNodeSize() > 1) {
    // create a message
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);

    int sendToPE = 1;

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  else if (CmiMyNodeSize() == 1) {
    printf("Only one node, send self test\n");
    // create a message
    Message *msg = new Message;
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);

    int sendToPE = 0;

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  CpvInitialize(int, exitHandlerId);
  CpvAccess(exitHandlerId) = CmiRegisterHandler(stop_handler);
  CpvInitialize(int, nodeHandlerId);
  CpvAccess(nodeHandlerId) = CmiRegisterHandler(nodeQueueTest);
  CpvInitialize(int, beginIdleId);
  CpvAccess(beginIdleId) =
      CcdCallOnCondition(CcdPROCESSOR_BEGIN_IDLE, beginIdle, 0);
  CpvInitialize(int, stillIdleId);
  CpvAccess(stillIdleId) =
      CcdCallOnCondition(CcdPROCESSOR_STILL_IDLE, stillIdle, 0);
  CpvInitialize(int, longIdleId);
  CpvAccess(longIdleId) =
      CcdCallOnCondition(CcdPROCESSOR_LONG_IDLE, longIdle, 0);

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
