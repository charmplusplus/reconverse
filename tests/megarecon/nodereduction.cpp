// nodereduction: a node broadcast asks one PE per node to contribute
// (node+1) to a CmiNodeReduce; node 0 checks the sum. The CmiNodeReduceStruct
// phase of megacon is dropped for the same reason as in reduction.cpp.
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
  if (CmiMyNode() != 0)
    CmiAbort("nodereduction: result delivered off node 0\n");
  for (int i = 0; i < CmiNumNodes(); ++i)
    sum += i + 1;
  if (m->sum != sum)
    CmiAbort("nodereduction: sum not matching: received %d, expecting %d\n",
             m->sum, sum);
  CmiFree(m);
  megarecon_ack();
}

static void broadcast_msg(void *vm) {
  mesg *m = (mesg *)vm;
  m->sum = CmiMyNode() + 1;
  CmiSetHandler(m, CpvAccess(reduction_msg_idx));
  CmiNodeReduce(m, sizeof(mesg), addMessage);
}

void nodereduction_init(void) {
  mesg *msg = (mesg *)CmiAlloc(sizeof(mesg));
  CmiSetHandler(msg, CpvAccess(broadcast_msg_idx));
  CmiSyncNodeBroadcastAllAndFree(sizeof(mesg), msg);
}

void nodereduction_moduleinit(void) {
  CpvInitialize(int, broadcast_msg_idx);
  CpvInitialize(int, reduction_msg_idx);
  CpvAccess(broadcast_msg_idx) = CmiRegisterHandler(broadcast_msg);
  CpvAccess(reduction_msg_idx) = CmiRegisterHandler(reduction_msg);
}
