// bigmsg: a one-megabyte message travels once around the PEs; PE 0 checks
// every word on return. Needs at least 2 PEs.
#include "megarecon.h"

#define CmiMsgHeaderSizeInts                                                   \
  ((CmiMsgHeaderSizeBytes + sizeof(int) - 1) / sizeof(int))
#define BIGMSG_INTS 250000

CpvDeclare(int, bigmsg_index);

static void bigmsg_handler(void *vmsg) {
  int *msg = (int *)vmsg;
  if (CmiMyPe() == 0) {
    for (int i = CmiMsgHeaderSizeInts; i < BIGMSG_INTS; i++)
      if (msg[i] != i)
        CmiAbort("Failure in bigmsg test, data corrupted.\n");
    CmiFree(msg);
    megarecon_ack();
  } else {
    int next = (CmiMyPe() + 1) % CmiNumPes();
    CmiSyncSendAndFree(next, BIGMSG_INTS * sizeof(int), msg);
  }
}

void bigmsg_init(void) {
  if (CmiNumPes() < 2) {
    CmiPrintf("note: bigmsg requires at least 2 processors, skipping test.\n");
    megarecon_ack();
    return;
  }
  int *msg = (int *)CmiAlloc(BIGMSG_INTS * sizeof(int));
  for (int i = CmiMsgHeaderSizeInts; i < BIGMSG_INTS; i++)
    msg[i] = i;
  CmiSetHandler(msg, CpvAccess(bigmsg_index));
  CmiSyncSendAndFree(1, BIGMSG_INTS * sizeof(int), msg);
}

void bigmsg_moduleinit(void) {
  CpvInitialize(int, bigmsg_index);
  CpvAccess(bigmsg_index) = CmiRegisterHandler(bigmsg_handler);
}
