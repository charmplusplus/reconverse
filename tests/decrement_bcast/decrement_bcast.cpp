// Multi-PE reconverse test for CmiCreateDecrementToEnqueue / CmiDecrementCounter
// Intended to run with an arbitrary number of PEs.
// Assumption: the counter is created on PE 0 with initialCount = 4 * CmiNumPes()
// (the user requested "4*CmiMyPe()" but that would be zero on PE 0; to make the
// test meaningful across arbitrary PEs we initialize to 4 * number of PEs so
// each PE can send 4 messages to PE 0).

#include "converse.h"
#include <string.h>

// Global pointer to the DecrementToEnqueueMsg created on PE 0. Other PEs do not
// need direct access to its fields; PE 0 will own and use this pointer when
// decrementing. Making it global simplifies the broadcast message.
static DecrementToEnqueueMsg *g_dte = NULL;
static int g_decInvH = -1;

// Handler called when final message is delivered (counter reached zero).
void exit_handler(void *msg) {
  CmiPrintf("Exit handler: counter reached zero on PE %d\n", CmiMyPe());
  CmiFreeDecrementToEnqueue(g_dte);
  CmiExit(0);
}

// Handler that will be invoked on PE0 for each incoming decrement-invoker
// message. It calls CmiDecrementCounter on the global DTE.
void decrement_invoker(void *msg) {
  (void)msg; // incoming message payload is not used

  //CmiPrintf("decrement_invoker: PE %d decrementing counter\n", CmiMyPe());

  // Call the decrement operation on the global DTE
  CmiDecrementCounter(g_dte);

  // Free the incoming message
  CmiFree(msg);
}

// Handler invoked on every PE when the broadcast arrives. Each PE will send 4
// small messages to PE 0; those messages will trigger decrement_invoker on PE0.
void broadcast_handler(void *msg) {
  // The broadcast carries no extra payload for this test; simply send 4
  // messages to PE 0.  PE 0 owns the global DTE (g_dte) and will perform the
  // decrements when it receives these messages.
  (void)msg; // unused

  int dest = 0;

  // Send 4 messages to PE 0. The messages themselves carry no useful payload
  // other than the header and are used only to trigger the decrement handler
  // on PE 0.
  //CmiPrintf("broadcast_handler: PE %d sending 4 decrement messages to PE 0\n", CmiMyPe());
  for (int i = 0; i < 4; ++i) {
    int sendSize = (int)sizeof(CmiMessageHeader);
    char *smsg = (char *)CmiAlloc(sendSize);
    memset(smsg, 0, sendSize);

    // set handler to the pre-registered decrement_invoker
    CmiSetHandler(smsg, g_decInvH);
    CmiMessageHeader *sendHdr = (CmiMessageHeader *)smsg;
    sendHdr->destPE = dest;
    sendHdr->messageSize = sendSize;

    // send to PE 0 and free the buffer if the send copies it; use SyncSendAndFree
    CmiSyncSendAndFree(dest, sendSize, smsg);
  }
}

// Start function: PE 0 creates the DecrementToEnqueueMsg with initialCount =
// 4 * CmiNumPes(), then broadcasts a message carrying the dte pointer. All
// PEs will receive the broadcast and send 4 messages to PE 0 which trigger
// decrements.
void test_start(int argc, char **argv) {
  (void)argc; (void)argv;
  int exitH = CmiRegisterHandler((CmiHandler)exit_handler);
  int bcastH = CmiRegisterHandler((CmiHandler)broadcast_handler);

  // register the decrement invoker once and store its handler globally so
  // broadcast_handler can reuse it when composing outgoing messages.
  g_decInvH = CmiRegisterHandler((CmiHandler)decrement_invoker);

  int numPes = CmiNumPes();
  int initial = 4 * numPes; // 4 messages per PE

  if (CmiMyPe() == 0) {
    // create final message that will be sent when counter reaches zero
    int finalSize = (int)sizeof(CmiMessageHeader);
    void *finalMsg = CmiAlloc(finalSize);
    memset(finalMsg, 0, finalSize);
    CmiSetHandler(finalMsg, exitH);
    CmiMessageHeader *fhdr = (CmiMessageHeader *)finalMsg;
    fhdr->destPE = 0;
    fhdr->messageSize = finalSize;

    g_dte = CmiCreateDecrementToEnqueue((unsigned int)initial, finalMsg);
    // Ensure stores to g_dte and its internals are visible to other PEs/threads.
    //CmiMemoryWriteFence();
    //CmiPrintf("[PE %d] created g_dte=%p, counter=%p (initial=%d)\n", CmiMyPe(), (void*)g_dte, (void*)(g_dte?g_dte->counter:NULL), initial);
  }

  // Ensure that PE 0 has finished creating g_dte before anyone reacts to the
  // broadcast. This prevents races where receivers send messages that reach
  // PE 0 before g_dte is initialized, which would cause CmiDecrementCounter to
  // see a null counter.
  CmiNodeAllBarrier();
  //CmiPrintf("[PE %d] passed node barrier\n", CmiMyPe());

  // Build a small broadcast message. Receivers will consult the global g_dte
  // (which is valid on PE 0) and send messages to PE 0 to trigger decrements.
  int bsize = (int)sizeof(CmiMessageHeader);
  void *bmsg = CmiAlloc(bsize);
  memset(bmsg, 0, bsize);
  CmiMessageHeader *bhdr = (CmiMessageHeader *)bmsg;
  bhdr->messageSize = bsize;
  CmiSetHandler(bmsg, bcastH);

  // Broadcast to all PEs
  if (CmiMyPe() == 0) CmiSyncBroadcastAllAndFree(bsize, bmsg);

  // Return from start; scheduler will process incoming messages and the exit
  // handler will terminate when the counter reaches zero.
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, test_start, 0, 0);
  return 0;
}
