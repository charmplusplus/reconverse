// broadc: three broadcast-to-all rounds (one from a stack message, two
// with AndFree), each PE replying to the originator, which counts replies.
#include "megarecon.h"
#include <cstdlib>

struct bchare {
  int totalsent;
  int totalreplies;
};

struct mesg {
  char head[CmiMsgHeaderSizeBytes];
  int reply_pe;
  bchare *reply_ptr;
  int magic;
};

CpvDeclare(int, broadc_recv_idx);
CpvDeclare(int, broadc_reply_idx);

static void broadc_recv(void *vm) {
  mesg *m = (mesg *)vm;
  if (m->magic != 0x12345678)
    CmiAbort("broadc failed.\n");
  CmiSetHandler(m, CpvAccess(broadc_reply_idx));
  CmiSyncSendAndFree(m->reply_pe, sizeof(mesg), m);
}

static void broadc_start_cycle(bchare *c) {
  switch (c->totalsent) {
  case 0: {
    mesg m = {{0}, CmiMyPe(), c, 0x12345678};
    CmiInitMsgHeader(m.head, sizeof(mesg));
    CmiSetHandler(&m, CpvAccess(broadc_recv_idx));
    CmiSyncBroadcastAll(sizeof(mesg), &m);
    c->totalsent++;
    break;
  }
  case 1:
  case 2: {
    mesg *mp = (mesg *)CmiAlloc(sizeof(mesg));
    CmiSetHandler(mp, CpvAccess(broadc_recv_idx));
    mp->reply_ptr = c;
    mp->reply_pe = CmiMyPe();
    mp->magic = 0x12345678;
    CmiSyncBroadcastAllAndFree(sizeof(mesg), mp);
    c->totalsent++;
    break;
  }
  case 3:
    free(c);
    megarecon_ack();
  }
}

static void broadc_reply(void *vm) {
  mesg *m = (mesg *)vm;
  if (m->magic != 0x12345678)
    CmiAbort("broadc failed.\n");
  bchare *c = m->reply_ptr;
  c->totalreplies++;
  if ((c->totalreplies % CmiNumPes()) == 0)
    broadc_start_cycle(c);
  CmiFree(m);
}

void broadc_init(void) {
  bchare *c = (bchare *)malloc(sizeof(bchare));
  c->totalsent = 0;
  c->totalreplies = 0;
  broadc_start_cycle(c);
}

void broadc_moduleinit(void) {
  CpvInitialize(int, broadc_recv_idx);
  CpvInitialize(int, broadc_reply_idx);
  CpvAccess(broadc_recv_idx) = CmiRegisterHandler(broadc_recv);
  CpvAccess(broadc_reply_idx) = CmiRegisterHandler(broadc_reply);
}
