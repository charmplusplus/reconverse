#include "converse.h"
#include <pthread.h>
#include <stdio.h>

CpvDeclare(int, test);
CpvDeclare(int, exitHandlerId);
CpvDeclare(int, nodeHandlerId);

struct Message {
  CmiMessageHeader header;
};

void stop_handler(void *vmsg) { 
  CsdExitScheduler(); 
  return;
}

void nodeQueueTest(void *msg) {
  printf("NODE QUEUE TEST on pe %d\n", CmiMyPe());
  for (int i = 0; i < CmiMyNodeSize(); i++) {
    Message *msg = new Message;
    msg->header.handlerId = CpvAccess(exitHandlerId);
    msg->header.messageSize = sizeof(Message);

    CmiSyncSendAndFree(i + CmiMyNodeSize() * CmiMyNode(), msg->header.messageSize, msg);
  }
}

void ping_handler(void *vmsg) {
  printf("PING HANDLER CALLED on pe %d\n", CmiMyPe());
  Message *msg = new Message;
  msg->header.handlerId = CpvAccess(nodeHandlerId);
  msg->header.messageSize = sizeof(Message);
  CmiSyncNodeSendAndFree(CmiMyNode(), msg->header.messageSize, msg);
}

CmiStartFn mymain(int argc, char **argv) {
  CpvInitialize(int, test);
  CpvAccess(test) = 42;

  printf("My PE is %d\n", CmiMyPe());

  int handlerId = CmiRegisterHandler(ping_handler);

  if (CmiMyRank() == 0 && CmiMyNodeSize() > 1) {
    // create a message
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);
    int sendToPE = 1 + CmiMyNodeSize() * CmiMyNode();

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  else if (CmiMyNodeSize() == 1) {
    printf("Only one node, send self test\n");
    // create a message
    Message *msg = new Message;
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);

    int sendToPE = CmiMyPe();

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  CpvInitialize(int, exitHandlerId);
  CpvAccess(exitHandlerId) = CmiRegisterHandler(stop_handler);
  CpvInitialize(int, nodeHandlerId);
  CpvAccess(nodeHandlerId) = CmiRegisterHandler(nodeQueueTest);

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
