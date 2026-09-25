// Exit with sends still in flight (reconverse-only reproducer).
//
// Every PE posts many large CmiSyncSendAndFree messages to PEs of OTHER
// processes and then leaves the scheduler at once, without waiting for
// anything. The send buffers come from the sending PE's mempool and are
// released by the LCI send-completion callback (CommLocalHandler ->
// CmiFree) whenever progress() gets to them. In ConverseExit every non-zero
// rank runs comm_backend::exitThread(), which destroys that PE's mempool,
// while rank 0 keeps calling progress() until all ranks are done; a
// completion for a buffer whose pool is already gone then calls CmiFree on
// freed (possibly unmapped) memory. Seen as a ~1-in-3 SIGSEGV at exit in
// CmiFree <- lci::progress_send <- CommBackendLCI2::progress <-
// ConverseExit after a broadcast-heavy Charm++ run on Delta.
//
// The receiving side only frees what arrives. Nothing here checks
// delivery; the test passes if the run exits normally.
//
// Usage: exit_inflight [messages per PE = 400] [bytes = 65536]
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

static int sink_handler;

static void sink(void *msg) { CmiFree(msg); }

static void start(int argc, char **argv) {
  sink_handler = CmiRegisterHandler(sink);
  int count = argc > 1 ? atoi(argv[1]) : 400;
  int bytes = argc > 2 ? atoi(argv[2]) : 65536;
  if (bytes < (int)sizeof(CmiMessageHeader)) bytes = sizeof(CmiMessageHeader);

  if (CmiNumNodes() < 2) {
    if (CmiMyPe() == 0)
      CmiPrintf("exit_inflight: needs at least 2 processes; nothing to do\n");
    CsdExitScheduler();
    return;
  }

  // Spread the sends over every PE that lives in another process.
  int sent = 0;
  for (int i = 0; sent < count; i++) {
    int dest = (CmiMyPe() + 1 + i) % CmiNumPes();
    if (CmiNodeOf(dest) == CmiMyNode()) continue;
    char *m = (char *)CmiAlloc(bytes);
    CmiSetHandler(m, sink_handler);
    CmiSyncSendAndFree(dest, bytes, m);
    sent++;
  }
  if (CmiMyPe() == 0)
    CmiPrintf("exit_inflight: %d PEs in %d processes, %d x %d-byte sends per PE, "
              "exiting now\n", CmiNumPes(), CmiNumNodes(), count, bytes);
  CsdExitScheduler();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, start, 0, 0);
  return 0;
}
