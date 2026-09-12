// multicast: like broadc, but through a CmiGroup holding every PE, with
// CmiSyncMulticast and CmiSyncMulticastAndFree.
#include "megarecon.h"
#include <cstdlib>

struct bchare {
  CmiGroup grp;
  int totalsent;
  int totalreplies;
};

struct mesg {
  char head[CmiMsgHeaderSizeBytes];
  int reply_pe;
  bchare *reply_ptr;
  int magic;
};

CpvDeclare(int, multicast_recv_idx);
CpvDeclare(int, multicast_reply_idx);

static void multicast_recv(void *vm) {
  mesg *m = (mesg *)vm;
  if (m->magic != 0x12345678)
    CmiAbort("multicast failed.\n");
  CmiSetHandler(m, CpvAccess(multicast_reply_idx));
  CmiSyncSendAndFree(m->reply_pe, sizeof(mesg), m);
}

static void multicast_start_cycle(bchare *c) {
  switch (c->totalsent) {
  case 0: {
    mesg m = {{0}, CmiMyPe(), c, 0x12345678};
    CmiInitMsgHeader(m.head, sizeof(mesg));
    CmiSetHandler(&m, CpvAccess(multicast_recv_idx));
    CmiSyncMulticast(c->grp, sizeof(mesg), &m);
    c->totalsent++;
    break;
  }
  case 1:
  case 2: {
    mesg *mp = (mesg *)CmiAlloc(sizeof(mesg));
    CmiSetHandler(mp, CpvAccess(multicast_recv_idx));
    mp->reply_ptr = c;
    mp->reply_pe = CmiMyPe();
    mp->magic = 0x12345678;
    CmiSyncMulticastAndFree(c->grp, sizeof(mesg), mp);
    c->totalsent++;
    break;
  }
  case 3:
    free(c);
    megarecon_ack();
  }
}

static void multicast_reply(void *vm) {
  mesg *m = (mesg *)vm;
  if (m->magic != 0x12345678)
    CmiAbort("multicast failed.\n");
  bchare *c = m->reply_ptr;
  c->totalreplies++;
  if ((c->totalreplies % CmiNumPes()) == 0)
    multicast_start_cycle(c);
  CmiFree(m);
}

static CmiGroup multicast_all(void) {
  int npes = CmiNumPes();
  int *pes = (int *)malloc(npes * sizeof(int));
  for (int i = 0; i < npes; i++)
    pes[i] = i;
  CmiGroup grp = CmiEstablishGroup(npes, pes);
  free(pes);
  return grp;
}

void multicast_init(void) {
  bchare *c = (bchare *)malloc(sizeof(bchare));
  c->grp = multicast_all();
  c->totalsent = 0;
  c->totalreplies = 0;
  multicast_start_cycle(c);
}

void multicast_moduleinit(void) {
  CpvInitialize(int, multicast_recv_idx);
  CpvInitialize(int, multicast_reply_idx);
  CpvAccess(multicast_recv_idx) = CmiRegisterHandler(multicast_recv);
  CpvAccess(multicast_reply_idx) = CmiRegisterHandler(multicast_reply);
}
