#include "converse.h"
#include <stdio.h>
#include <pthread.h>

CpvDeclare(int, test);
CpvDeclare(int, exitHandlerId);
CpvDeclare(int, nodeHandlerId);

struct Message
{
  CmiMessageHeader header;
};

void stop_handler(void *vmsg)
{
  CsdExitScheduler();
}

void nodeQueueTest(void *msg)
{
  printf("NODE QUEUE TEST on pe %d\n", CmiMyRank());
  for (int i = 0; i < CmiMyNodeSize(); i++)
  {
    Message *msg = new Message;
    msg->header.handlerId = CpvAccess(exitHandlerId);
    msg->header.messageSize = sizeof(Message);
    msg->header.destPE = i;

    CmiSyncSendAndFree(i, msg->header.messageSize, msg);
  }
}

void ping_handler(void *vmsg)
{
  printf("PING HANDLER CALLED\n");
  Message *msg = new Message;
  msg->header.handlerId = CpvAccess(nodeHandlerId);
  msg->header.messageSize = sizeof(Message);
  CmiSyncNodeSendAndFree(0, msg->header.messageSize, msg);
}

CmiStartFn mymain(int argc, char **argv)
{
  CpvInitialize(int, test);
  CpvAccess(test) = 42;

  printf("My PE is %d\n", CmiMyRank());

  int handlerId = CmiRegisterHandler(ping_handler);

  if (CmiMyRank() == 0 && CmiMyNodeSize() > 1)
  {
    // create a message
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);
    msg->header.destPE = 1;

    // TODO: why is this info passed within message an also separately
    int sendToPE = 1;

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  else if (CmiMyNodeSize() == 1)
  {
    printf("Only one node, send self test\n");
    // create a message
    Message *msg = new Message;
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);
    msg->header.destPE = 1;

    // TODO: why is this info passed within message an also separately
    int sendToPE = 0;

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  CpvInitialize(int, exitHandlerId);
  CpvAccess(exitHandlerId) = CmiRegisterHandler(stop_handler);
  CpvInitialize(int, nodeHandlerId);
  CpvAccess(nodeHandlerId) = CmiRegisterHandler(nodeQueueTest);

  // printf("Answer to the Ultimate Question of Life, the Universe, and Everything: %d\n", CpvAccess(test));
  return 0;
}

int main(int argc, char **argv)
{
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}