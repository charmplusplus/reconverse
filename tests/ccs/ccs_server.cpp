// CCS server side of the test: answers "ping" requests addressed to one PE,
// to every PE (broadcast) or to a list of PEs (multicast), the same through
// a delayed reply for "dping", and exits on "quit". Run with
// ++server-port <port> (0 lets the OS pick; the port is printed) and drive
// it with ccs_client.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "converse.h"
#include "conv-ccs.h"

static void ping_handler(char *msg) {
  const char *name = msg + CmiMsgHeaderSizeBytes;
  char answer[128];
  int len = snprintf(answer, sizeof(answer), "hello %s from processor %d\n",
                     name, CmiMyPe());
  // no terminating NUL: broadcast and multicast replies are concatenated
  CcsSendReply(len, answer);
  CmiFree(msg);
}

// "dping": the same answer, sent later with CcsDelayReply from a message to
// this PE, after the request handler and its message are gone
static int dpingAnswerIdx;
struct DelayedPing {
  char header[CmiMsgHeaderSizeBytes];
  CcsDelayedReply reply;
};

static void dping_answer(char *msg) {
  DelayedPing *d = (DelayedPing *)msg;
  char answer[128];
  int len = snprintf(answer, sizeof(answer), "hello delayed from processor %d\n",
                     CmiMyPe());
  CcsSendDelayedReply(d->reply, len, answer);
  CmiFree(msg);
}

static void dping_handler(char *msg) {
  DelayedPing *d = (DelayedPing *)CmiAlloc(sizeof(DelayedPing));
  d->reply = CcsDelayReply();
  CmiSetHandler(d, dpingAnswerIdx);
  CmiSyncSendAndFree(CmiMyPe(), sizeof(DelayedPing), d);
  CmiFree(msg);
}

static void quit_handler(char *msg) {
  CcsSendReply(0, NULL);
  CmiFree(msg);
  CmiExit(0);
}

static void start(int argc, char **argv) {
  CcsRegisterHandler("ping", (CmiHandler)ping_handler);
  CcsSetMergeFn("ping", CcsMerge_concat);
  dpingAnswerIdx = CmiRegisterHandler((CmiHandler)dping_answer);
  CcsRegisterHandler("dping", (CmiHandler)dping_handler);
  CcsSetMergeFn("dping", CcsMerge_concat);
  // no merge function: broadcasts and multicasts to it must be turned down
  CcsRegisterHandler("nomerge", (CmiHandler)ping_handler);
  CcsRegisterHandler("quit", (CmiHandler)quit_handler);
  if (CmiMyPe() == 0) {
    printf("ccs_server: %d PEs, handlers registered\n", CmiNumPes());
    fflush(stdout);
  }
}

int main(int argc, char **argv) { ConverseInit(argc, argv, start, 0, 0); }
