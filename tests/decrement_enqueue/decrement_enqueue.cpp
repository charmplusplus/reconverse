// Simple reconverse test for CmiCreateDecrementToEnqueue / CmiDecrementCounter
// Intended to run with 1 PE on 1 process.

#include "converse.h"
#include <string.h>

DecrementToEnqueueMsg *dte;

// Handler invoked when the decrement counter reaches zero. Exits the program.
void exit_handler(void *msg) {
  // free DecrementToEnqueueMsg
  CmiFreeDecrementToEnqueue(dte);
  CmiPrintf("Exit handler called, exiting...\n");
  CmiExit(0);
}

// Start function invoked by ConverseInit.
void test_start(int argc, char **argv) {
  CmiPrintf("Starting decrement_enqueue test...\n");
  // Only PE 0 will create and drive the counter per the test spec.
  if (CmiMyPe() != 0) return;

  int handler = CmiRegisterHandler((CmiHandler)exit_handler);

  // Create a minimal message consisting only of the message header.
  int msgSize = (int)sizeof(CmiMessageHeader);
  void *msg = CmiAlloc(msgSize);
  memset(msg, 0, msgSize);

  // Set handler, destination and size on the header so CmiDecrementCounter
  // can inspect them when it enqueues the message.
  CmiSetHandler(msg, handler);
  CmiMessageHeader *hdr = (CmiMessageHeader *)msg;
  hdr->destPE = (CmiUInt4)CmiMyPe();
  hdr->messageSize = msgSize;

  // Create the decrement-to-enqueue helper with initial count 16.
  dte = CmiCreateDecrementToEnqueue(msg, 16u);

  // Decrement 16 times; on the 16th call the message will be sent and the
  // registered handler will call CmiExit.
  for (int i = 0; i < 16; ++i) {
    CmiDecrementCounter(dte);
  }

  // Return from start; scheduler will run and the exit handler will stop it.
}

int main(int argc, char **argv) {
  // Start the Converse runtime with our test_start function.
  ConverseInit(argc, argv, test_start, 0, 0);
  return 0;
}
