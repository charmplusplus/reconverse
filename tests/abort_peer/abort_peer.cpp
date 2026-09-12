// Reconverse-only reproducer probe: one PE calls CmiAbort while all PEs
// are exchanging messages. Purpose: observe how the surviving processes
// behave when a peer aborts mid-communication (clean teardown vs hang vs
// send-path assertions). Written while investigating an intermittent
// "backend_ofi_inline.hpp:post_send_impl ... Invalid argument" assertion
// seen in a Charm++ application at 32 processes when one rank aborted
// during startup.
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

static int ping_handlerID;
static int kIterations =
    1000000; // effectively unbounded; run is externally capped
static int kAbortIter = 50;
static int kAbortPe = 1;

struct Message {
  CmiMessageHeader header;
  int iter;
};

static void send_next(int iter) {
  int dest = (CmiMyPe() + 1) % CmiNumPes();
  Message *m = (Message *)CmiAlloc(sizeof(Message));
  m->header.handlerId = ping_handlerID;
  m->header.messageSize = sizeof(Message);
  m->iter = iter;
  CmiSyncSendAndFree(dest, sizeof(Message), m);
}

static void ping_handler(void *vmsg) {
  Message *msg = (Message *)vmsg;
  int iter = msg->iter;
  CmiFree(msg);
  if (iter % 50 == 0) {
    printf("[pe %d] handler iter %d\n", CmiMyPe(), iter);
    fflush(stdout);
  }
  if (CmiMyPe() == kAbortPe && iter == kAbortIter) {
    printf("[pe %d] calling CmiAbort at iteration %d\n", CmiMyPe(), iter);
    fflush(stdout);
    CmiAbort("abort_peer: deliberate mid-run abort");
  }
  if (iter >= kIterations) {
    if (CmiMyPe() == 0) {
      printf("abort_peer: completed %d iterations (should NOT happen when "
             "the abort fires)\n",
             iter);
      fflush(stdout);
      CmiExit(0);
    }
    return;
  }
  send_next(iter + 1);
}

CmiStartFn mymain(int argc, char **argv) {
  ping_handlerID = CmiRegisterHandler((CmiHandler)ping_handler);
  if (argc > 1)
    kAbortIter = atoi(argv[1]);
  printf("[pe %d] mymain: handler id %d, npes %d\n", CmiMyPe(), ping_handlerID,
         CmiNumPes());
  fflush(stdout);
  // Every PE starts a ring: N concurrent rings keep traffic flowing
  // through the aborting PE's neighbors after it dies — the send-to-dead-
  // endpoint scenario under test.
  send_next(0);
  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
}
