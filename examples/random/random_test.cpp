#include "converse.h"
#include <stdio.h>
#include <pthread.h>

struct Message
{
  CmiMessageHeader header;
};


void ping_handler(void *vmsg)
{
  CrnSrand(100);
  CmiPrintf("Next random int: %d\n", CrnRand());
  CmiPrintf("Next random double: %f\n", CrnDrand());
  CmiExit(0);
}

CmiStartFn mymain(int argc, char **argv)
{

  printf("My PE is %d\n", CmiMyRank());

  int handlerId = CmiRegisterHandler(ping_handler);

  if (CmiMyRank() == 0 && CmiMyNodeSize() > 1)
  {
    // create a message
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);

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

    int sendToPE = 0;

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  return 0;
}

int main(int argc, char **argv)
{
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
