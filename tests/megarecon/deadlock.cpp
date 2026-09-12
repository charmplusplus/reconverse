// deadlock: PEs 0 and 1 each fire 5000 small messages at the other without
// ever polling, then a closing message carrying -count; the receiver's sum
// returns to zero exactly once. A runtime whose send blocks on a full
// channel while the peer is also sending deadlocks here. Needs 2 PEs.
#include "megarecon.h"

struct incmsg {
  char head[CmiMsgHeaderSizeBytes];
  int n;
};

CpvDeclare(int, deadlock_inc_idx);
CpvDeclare(int, deadlock_cram_idx);
CpvDeclare(int, deadlock_count);

static void deadlock_inc(void *vm) {
  incmsg *m = (incmsg *)vm;
  CpvAccess(deadlock_count) += m->n;
  if (CpvAccess(deadlock_count) == 0)
    megarecon_ack();
  CmiFree(m);
}

static void deadlock_cram(void *msg) {
  incmsg m = {{0}, 1};
  CmiInitMsgHeader(m.head, sizeof(incmsg));
  CmiSetHandler(&m, CpvAccess(deadlock_inc_idx));
  int count = 0;
  while (count < 5000) {
    CmiSyncSend(1 - CmiMyPe(), sizeof(m), &m);
    count++;
  }
  m.n = -count;
  CmiSyncSend(1 - CmiMyPe(), sizeof(m), &m);
  CmiFree(msg);
}

void deadlock_init(void) {
  if (CmiNumPes() < 2) {
    CmiPrintf("warning: need 2 processors for deadlock-test, skipping.\n");
    megarecon_ack();
    megarecon_ack();
    return;
  }
  incmsg msg = {{0}, 0};
  CmiInitMsgHeader(msg.head, sizeof(incmsg));
  CmiSetHandler(&msg, CpvAccess(deadlock_cram_idx));
  CmiSyncSend(0, sizeof(msg), &msg);
  CmiSyncSend(1, sizeof(msg), &msg);
}

void deadlock_moduleinit(void) {
  CpvInitialize(int, deadlock_inc_idx);
  CpvInitialize(int, deadlock_cram_idx);
  CpvInitialize(int, deadlock_count);
  CpvAccess(deadlock_inc_idx) = CmiRegisterHandler(deadlock_inc);
  CpvAccess(deadlock_cram_idx) = CmiRegisterHandler(deadlock_cram);
  CpvAccess(deadlock_count) = 0;
}
