#include "converse.h"
#include <pthread.h>
#include <stdio.h>

struct Message {
  CmiMessageHeader header;
};

void ping_handler(void *vmsg) {
  CrnSrand(100);
  CmiPrintf("Next random int: %d\n", CrnRand());
  CmiPrintf("Next random double: %f\n", CrnDrand());

  // Ranged draws stay inside their range (the int range is inclusive, as in
  // classic Converse; the double range is half-open), and a reseed
  // reproduces the sequence.
  for (int i = 0; i < 1000; i++) {
    int r = CrnRandRange(10, 20);
    if (r < 10 || r > 20)
      CmiAbort("CrnRandRange(10,20) returned %d", r);
    double d = CrnDrandRange(-2.5, 2.5);
    if (d < -2.5 || d >= 2.5)
      CmiAbort("CrnDrandRange(-2.5,2.5) returned %f", d);
  }
  CrnSrand(7);
  int a = CrnRand(), b = CrnRandRange(0, 1000);
  double c = CrnDrand(), e = CrnDrandRange(1.0, 2.0);
  CrnSrand(7);
  if (a != CrnRand() || b != CrnRandRange(0, 1000) || c != CrnDrand() ||
      e != CrnDrandRange(1.0, 2.0))
    CmiAbort("reseeding did not reproduce the random sequence");
  CmiPrintf("ranged and reseeded draws ok\n");
  CmiExit(0);
}

CmiStartFn mymain(int argc, char **argv) {

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

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
