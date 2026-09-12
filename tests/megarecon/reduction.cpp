// reduction: a broadcast asks every PE to contribute (PE+1) to a CmiReduce
// of a message; PE 0 checks the sum. megacon had a second phase using
// CmiReduceStruct with pup_c, which reconverse does not provide; CmiReduce
// itself is also covered by the standalone reduction tests.
#include "megarecon.h"

struct mesg {
  char head[CmiMsgHeaderSizeBytes];
  int sum;
};

CpvStaticDeclare(int, broadcast_msg_idx);
CpvStaticDeclare(int, reduction_msg_idx);

static void *addMessage(int *size, void *data, void **remote, int count) {
  mesg *msg = (mesg *)data;
  for (int i = 0; i < count; ++i)
    msg->sum += ((mesg *)remote[i])->sum;
  return data;
}

static void reduction_msg(void *vm) {
  mesg *m = (mesg *)vm;
  int sum = 0;
  if (CmiMyPe() != 0)
    CmiAbort("reduction: result delivered off PE 0\n");
  for (int i = 0; i < CmiNumPes(); ++i)
    sum += i + 1;
  if (m->sum != sum)
    CmiAbort("reduction: sum not matching: received %d, expecting %d\n", m->sum,
             sum);
  CmiFree(m);
  megarecon_ack();
}

static void broadcast_msg(void *vm) {
  mesg *m = (mesg *)vm;
  m->sum = CmiMyPe() + 1;
  CmiSetHandler(m, CpvAccess(reduction_msg_idx));
  CmiReduce(m, sizeof(mesg), addMessage);
}

void reduction_init(void) {
  mesg *msg = (mesg *)CmiAlloc(sizeof(mesg));
  CmiSetHandler(msg, CpvAccess(broadcast_msg_idx));
  CmiSyncBroadcastAllAndFree(sizeof(mesg), msg);
}

void reduction_moduleinit(void) {
  CpvInitialize(int, broadcast_msg_idx);
  CpvInitialize(int, reduction_msg_idx);
  CpvAccess(broadcast_msg_idx) = CmiRegisterHandler(broadcast_msg);
  CpvAccess(reduction_msg_idx) = CmiRegisterHandler(reduction_msg);
}
