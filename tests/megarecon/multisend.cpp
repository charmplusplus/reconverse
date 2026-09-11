// multisend: eight messages of decreasing size, each bound to its own
// CmiRegisterHandlerEx handler carrying a per-handler user pointer, are sent
// to one PE, checked there, and returned. megacon sent them in one
// CmiMultipleSend call; reconverse has no CmiMultipleSend, so they are sent
// one at a time. The first message lives on the stack.
#include "megarecon.h"

struct multisend_info {
  int me;
};

struct multisendmsg {
  char core[CmiMsgHeaderSizeBytes];
  int me;
  double data[10];
};

#define nMulti 8

CpvDeclare(int *, multisend_index);
CpvDeclare(int, multisend_done_index);
CpvDeclare(int, multisend_replies);

static void checkMsg(multisendmsg *msg) {
  for (int i = 0; i < 10 - msg->me; i++)
    if (msg->data[i] != (double)(i + msg->me))
      CmiAbort("data corrupted in multisend.\n");
}

static void multisend_handler(void *vmsg, void *vinfo) {
  multisendmsg *msg = (multisendmsg *)vmsg;
  multisend_info *info = (multisend_info *)vinfo;
  if (msg->me != info->me)
    CmiAbort("multisend: message reached the wrong handler.\n");
  checkMsg(msg);
  CmiSetHandler(msg, CpvAccess(multisend_done_index));
  CmiSyncSendAndFree(0, sizeof(multisendmsg) - sizeof(double) * msg->me, msg);
}

static void multisend_done_handler(void *vmsg) {
  multisendmsg *msg = (multisendmsg *)vmsg;
  checkMsg(msg);
  CmiFree(msg);
  CpvAccess(multisend_replies)++;
  if (CpvAccess(multisend_replies) == nMulti)
    megarecon_ack();
}

void multisend_init(void) {
  int dest = 1 % CmiNumPes();
  multisendmsg first;
  CpvAccess(multisend_replies) = 0;
  for (int m = 0; m < nMulti; m++) {
    multisendmsg *msg =
        (m == 0) ? &first : (multisendmsg *)CmiAlloc(sizeof(multisendmsg));
    int size = sizeof(multisendmsg) - sizeof(double) * m;
    if (m == 0)
      CmiInitMsgHeader(first.core, size);
    CmiSetHandler(msg, CpvAccess(multisend_index)[m]);
    msg->me = m;
    for (int i = 0; i < 10 - m; i++)
      msg->data[i] = (double)(i + m);
    if (m == 0)
      CmiSyncSend(dest, size, msg);
    else
      CmiSyncSendAndFree(dest, size, msg);
  }
}

void multisend_moduleinit(void) {
  CpvInitialize(int *, multisend_index);
  CpvInitialize(int, multisend_done_index);
  CpvInitialize(int, multisend_replies);
  CpvAccess(multisend_index) = (int *)CmiAlloc(nMulti * sizeof(int));
  for (int m = 0; m < nMulti; m++) {
    multisend_info *i = (multisend_info *)CmiAlloc(sizeof(multisend_info));
    i->me = m;
    CpvAccess(multisend_index)[m] = CmiRegisterHandlerEx(multisend_handler, i);
  }
  CpvAccess(multisend_done_index) = CmiRegisterHandler(multisend_done_handler);
}
