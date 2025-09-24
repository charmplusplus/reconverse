#include "converse.h"
#include <pthread.h>
#include <stdio.h>

CpvDeclare(int, exitHandlerId);

struct Message {
  CmiMessageHeader header;
};

void stop_handler(void *vmsg) { CsdExitScheduler(); }

void ping_handler(void *vmsg) {
  printf("PING HANDLER CALLED ON PE %i\n", CmiMyPe());
  stop_handler(NULL);
}

CmiStartFn mymain(int argc, char **argv) {
  printf("My PE is %d\n", CmiMyPe());

  int handlerId = CmiRegisterHandler(ping_handler);

  if (CmiMyPe() == 0) {
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    msg->header.handlerId = handlerId;
    msg->header.messageSize = sizeof(Message);

    CmiSyncBroadcastAllAndFree(msg->header.messageSize, msg);
  }

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
