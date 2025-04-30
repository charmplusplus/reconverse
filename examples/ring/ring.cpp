#include "converse.h"
#include <pthread.h>
#include <stdio.h>

CpvDeclare(int, test);

int ping_handlerID;
int payloadSize = 1 * sizeof(int);

struct Message {
  CmiMessageHeader header;
  int data[1];
};

void ping_handler(void *vmsg) {
  Message *msg = (Message *)vmsg;
  printf("PE %d pinged in ring with index %d.\n", CmiMyRank(), msg->data[0]);

  // test assert statements are working
  CmiAssert(CmiMyRank() == msg->header.destPE);

  if (CmiMyRank() != CmiMyNodeSize() - 1) {
    Message *newmsg = new Message;
    newmsg->header.handlerId = ping_handlerID;
    newmsg->header.messageSize = sizeof(Message);

    newmsg->data[0] = msg->data[0] + 1;

    CmiSyncSendAndFree(CmiMyRank() + 1, newmsg->header.messageSize, newmsg);
  } else {
    CmiExit(0);
  }
}

CmiStartFn mymain(int argc, char **argv) {
  CpvInitialize(int, test);
  CpvAccess(test) = 42;

  ping_handlerID = CmiRegisterHandler(ping_handler);

  if (CmiMyRank() == 0) {
    // create a message
    Message *msg = new Message;
    msg->header.handlerId = ping_handlerID;
    msg->header.messageSize = sizeof(Message);
    msg->data[0] = 0;

    int sendToPE = 0;

    // Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(sendToPE, msg->header.messageSize, msg);
  }

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
