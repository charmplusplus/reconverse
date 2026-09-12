// ringsimple: ten messages each travel 1000 hops around the PEs, from a
// stack-allocated original, checking their payload at every hop.
#include "megarecon.h"
#include <cstdlib>

#define entries 10

struct ringmsg {
  char core[CmiMsgHeaderSizeBytes];
  int hops, ringno;
  int data[10];
};

CpvDeclare(int, ringsimple_hop_index);

static void ringsimple_hop(void *vmsg) {
  ringmsg *msg = (ringmsg *)vmsg;
  int nextpe = (CmiMyPe() + 1) % CmiNumPes();
  for (int i = 0; i < 10; i++)
    if (msg->data[i] != i)
      CmiAbort("data corrupted in ringsimple_hop.\n");
  if (msg->hops) {
    msg->hops--;
    CmiSyncSendAndFree(nextpe, sizeof(ringmsg), msg);
  } else {
    megarecon_ack();
    CmiFree(msg);
  }
}

void ringsimple_init(void) {
  ringmsg msg = {{0}, 1000, 0, {0}};
  CmiInitMsgHeader(msg.core, sizeof(ringmsg));
  for (int i = 0; i < 10; i++)
    msg.data[i] = i;
  CmiSetHandler(&msg, CpvAccess(ringsimple_hop_index));
  for (int i = 0; i < entries; i++) {
    msg.ringno = i;
    CmiSyncSend(0, sizeof(ringmsg), &msg);
  }
}

void ringsimple_moduleinit(void) {
  CpvInitialize(int, ringsimple_hop_index);
  CpvAccess(ringsimple_hop_index) = CmiRegisterHandler(ringsimple_hop);
}
