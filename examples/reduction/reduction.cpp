#include "converse.h"
#include <pthread.h>
#include <stdio.h>

CpvDeclare(int, exitHandlerId);

struct Message {
  CmiMessageHeader header;
  int myReductionData;
};

void stop_handler(void *vmsg) { CsdExitScheduler(); }

void *mergeByAddition(int *size, void *data, void **remote, int n) {
  Message *localMessage = (Message *)data;
  int localData = localMessage->myReductionData;

  for (int i = 0; i < n; i++) {
    Message *remoteMessage = (Message *)remote[i];
    int remoteData = remoteMessage->myReductionData;
    localData += remoteData;
  }

  localMessage->myReductionData = localData;
  return localMessage;
}

void ping_handler(void *vmsg) {
  // compute expected result:
  int n = CmiNumPes() - 1;
  int expected = n * (n + 1) / 2;

  if (expected != ((Message *)vmsg)->myReductionData) {
    CmiAbort("Reduction result mismatch: expected %d, got %d\n", expected,
             ((Message *)vmsg)->myReductionData);
  }
  printf("Reduction done on %d. Result matches expected: %d.\n", CmiMyPe(),
         ((Message *)vmsg)->myReductionData);
  CmiExit(0);
}

CmiStartFn mymain(int argc, char **argv) {
  int handlerId = CmiRegisterHandler(ping_handler);

  // create a message
  Message *msg = new Message;
  msg->header.handlerId = handlerId;
  msg->header.messageSize = sizeof(Message);
  msg->myReductionData = CmiMyPe();

  CmiReduce(msg, sizeof(Message), mergeByAddition);
  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
