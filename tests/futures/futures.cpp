// Cfuture test. A thread on PE 0 creates one future per PE, mails each
// future's handle to its PE, and waits on them in order; every PE fills its
// future with a value derived from its PE number (PE 0 through the local
// path, the others through the remote path). The thread also exercises the
// zero-copy buffer form on a local future. The waits must return the
// expected values and the program must exit 0.
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HandleMsg {
  CmiMessageHeader header;
  Cfuture future;
};

static int handleHandlerID;
static Cfuture *futures;

static void fill_my_future(Cfuture f) {
  int value = 1000 + CmiMyPe();
  CfutureSet(f, &value, sizeof(value));
}

static void handle_handler(void *vmsg) {
  HandleMsg *msg = (HandleMsg *)vmsg;
  fill_my_future(msg->future);
  CmiFree(msg);
}

static void waiter_thread(void *) {
  int npes = CmiNumPes();
  futures = (Cfuture *)malloc(npes * sizeof(Cfuture));
  for (int pe = 0; pe < npes; pe++) {
    futures[pe] = CfutureCreate();
    if (pe == CmiMyPe()) {
      fill_my_future(futures[pe]);
    } else {
      HandleMsg *m = (HandleMsg *)CmiAlloc(sizeof(HandleMsg));
      CmiSetHandler(m, handleHandlerID);
      m->future = futures[pe];
      CmiSyncSendAndFree(pe, sizeof(HandleMsg), m);
    }
  }
  int bad = 0;
  for (int pe = 0; pe < npes; pe++) {
    int *v = (int *)CfutureWait(futures[pe]);
    if (*v != 1000 + pe) {
      printf("future %d: got %d, expected %d\n", pe, *v, 1000 + pe);
      bad++;
    }
    CfutureDestroy(futures[pe]);
  }
  // Zero-copy buffer form, local.
  Cfuture local = CfutureCreate();
  char *buf = (char *)CfutureCreateBuffer(16);
  strcpy(buf, "buffered");
  CfutureStoreBuffer(local, buf);
  if (strcmp((char *)CfutureWait(local), "buffered") != 0) {
    printf("buffer future: wrong contents\n");
    bad++;
  }
  CfutureDestroy(local);
  free(futures);
  if (bad)
    CmiAbort("futures: %d wrong values", bad);
  printf("futures: %d futures across %d PEs filled correctly\n", npes, npes);
  CmiExit(0);
}

CmiStartFn mymain(int argc, char **argv) {
  handleHandlerID = CmiRegisterHandler((CmiHandler)handle_handler);
  if (CmiMyPe() == 0)
    CthAwaken(CthCreate((CthVoidFn)waiter_thread, 0, 0));
  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
