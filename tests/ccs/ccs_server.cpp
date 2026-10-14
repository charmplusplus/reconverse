// CCS server side of the test: answers "ping" requests addressed to one PE,
// to every PE (broadcast) or to a list of PEs (multicast), and exits on
// "quit". Run with ++server-port <port> (0 lets the OS pick; the port is
// printed) and drive it with ccs_client.
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

static void quit_handler(char *msg) {
  CcsSendReply(0, NULL);
  CmiFree(msg);
  CmiExit(0);
}

static void start(int argc, char **argv) {
  CcsRegisterHandler("ping", (CmiHandler)ping_handler);
  CcsSetMergeFn("ping", CcsMerge_concat);
  CcsRegisterHandler("quit", (CmiHandler)quit_handler);
  if (CmiMyPe() == 0) {
    printf("ccs_server: %d PEs, handlers registered\n", CmiNumPes());
    fflush(stdout);
  }
}

int main(int argc, char **argv) { ConverseInit(argc, argv, start, 0, 0); }
