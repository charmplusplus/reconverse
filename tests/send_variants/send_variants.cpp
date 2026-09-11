// The send-family entry points Charm++ reaches through its middle layer but
// reconverse's suite never called: CmiSyncBroadcastFn / CmiFreeBroadcastFn
// (all PEs but me), CmiSyncBroadcastAllFn, CmiFreeBroadcastAllFn,
// CmiSyncNodeBroadcastAndFree (all nodes but mine), CmiFreeNodeSendFn,
// CmiSyncListSendFn / CmiFreeListSendFn, CmiSyncMulticastFn /
// CmiFreeMulticastFn, CmiSyncSendFn / CmiFreeSendFn. PE 0 issues each once
// with its expected delivery count, deliveries ack to PE 0, and PE 0 exits
// everyone when all counts match; an extra delivery aborts.
#include "converse.h"
#include <cstdio>
#include <cstring>

enum Kind {
  K_BCAST,
  K_FBCAST,
  K_BCAST_ALL,
  K_FBCAST_ALL,
  K_NODE_BCAST_FREE,
  K_FNODE_SEND,
  K_LIST,
  K_FLIST,
  K_MCAST,
  K_FMCAST,
  K_SEND,
  K_FSEND,
  K_COUNT
};
static const char *kindName[K_COUNT] = {"CmiSyncBroadcastFn",
                                        "CmiFreeBroadcastFn",
                                        "CmiSyncBroadcastAllFn",
                                        "CmiFreeBroadcastAllFn",
                                        "CmiSyncNodeBroadcastAndFree",
                                        "CmiFreeNodeSendFn",
                                        "CmiSyncListSendFn",
                                        "CmiFreeListSendFn",
                                        "CmiSyncMulticastFn",
                                        "CmiFreeMulticastFn",
                                        "CmiSyncSendFn",
                                        "CmiFreeSendFn"};

struct Msg {
  CmiMessageHeader header;
  int kind;
  int payload[4];
};

CpvDeclare(int, workIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);
static int expected[K_COUNT];
static int received[K_COUNT];
static int remaining;

static Msg *make(int kind) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  CmiSetHandler(m, CpvAccess(workIdx));
  m->kind = kind;
  for (int i = 0; i < 4; i++)
    m->payload[i] = 100 * kind + i;
  return m;
}

static void work_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  for (int i = 0; i < 4; i++)
    if (m->payload[i] != 100 * m->kind + i)
      CmiAbort("send variants: payload corrupted for %s", kindName[m->kind]);
  if ((m->kind == K_BCAST || m->kind == K_FBCAST) && CmiMyPe() == 0)
    CmiAbort("send variants: %s delivered to the sender", kindName[m->kind]);
  if (m->kind == K_NODE_BCAST_FREE && CmiMyNode() == 0)
    CmiAbort("send variants: %s delivered to the sender's node",
             kindName[m->kind]);
  m->header.handlerId = CpvAccess(ackIdx);
  CmiSyncSendAndFree(0, sizeof(Msg), m);
}

static void ack_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  int k = m->kind;
  CmiFree(m);
  received[k]++;
  if (received[k] > expected[k])
    CmiAbort("send variants: %s delivered %d times, expected %d", kindName[k],
             received[k], expected[k]);
  if (received[k] == expected[k]) {
    CmiPrintf("[0] %s: %d deliveries ok\n", kindName[k], received[k]);
    if (--remaining == 0) {
      Msg *e = make(0);
      CmiSetHandler(e, CpvAccess(exitIdx));
      CmiSyncBroadcastAllAndFree(sizeof(Msg), e);
    }
  }
}

static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void issue_all(void) {
  int npes = CmiNumPes(), nnodes = CmiNumNodes();
  int n = sizeof(Msg);
  Msg *m;

  m = make(K_BCAST);
  expected[K_BCAST] = npes - 1;
  CmiSyncBroadcastFn(n, (char *)m);
  CmiFree(m);
  expected[K_FBCAST] = npes - 1;
  CmiFreeBroadcastFn(n, (char *)make(K_FBCAST));

  m = make(K_BCAST_ALL);
  expected[K_BCAST_ALL] = npes;
  CmiSyncBroadcastAllFn(n, (char *)m);
  CmiFree(m);
  expected[K_FBCAST_ALL] = npes;
  CmiFreeBroadcastAllFn(n, (char *)make(K_FBCAST_ALL));

  expected[K_NODE_BCAST_FREE] = nnodes - 1;
  CmiSyncNodeBroadcastAndFree(n, make(K_NODE_BCAST_FREE));

  expected[K_FNODE_SEND] = 1;
  CmiFreeNodeSendFn(nnodes - 1, n, (char *)make(K_FNODE_SEND));

  int *pes = new int[npes];
  for (int i = 0; i < npes; i++)
    pes[i] = i;
  m = make(K_LIST);
  expected[K_LIST] = npes;
  CmiSyncListSendFn(npes, pes, n, (char *)m);
  CmiFree(m);
  expected[K_FLIST] = npes;
  CmiFreeListSendFn(npes, pes, n, (char *)make(K_FLIST));

  CmiGroup grp = CmiEstablishGroup(npes, pes);
  m = make(K_MCAST);
  expected[K_MCAST] = npes;
  CmiSyncMulticastFn(grp, n, (char *)m);
  CmiFree(m);
  expected[K_FMCAST] = npes;
  CmiFreeMulticastFn(grp, n, (char *)make(K_FMCAST));
  delete[] pes;

  m = make(K_SEND);
  expected[K_SEND] = 1;
  CmiSyncSendFn(npes - 1, n, (char *)m);
  CmiFree(m);
  expected[K_FSEND] = 1;
  CmiFreeSendFn(npes - 1, n, (char *)make(K_FSEND));

  remaining = 0;
  for (int k = 0; k < K_COUNT; k++)
    if (expected[k] > 0)
      remaining++;
  // with one PE or one node some kinds deliver nothing; count those as done
  for (int k = 0; k < K_COUNT; k++)
    if (expected[k] == 0)
      CmiPrintf("[0] %s: nothing to deliver here\n", kindName[k]);
  if (remaining == 0) {
    Msg *e = make(0);
    CmiSetHandler(e, CpvAccess(exitIdx));
    CmiSyncBroadcastAllAndFree(n, e);
  }
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, workIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvAccess(workIdx) = CmiRegisterHandler(work_handler);
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  if (CmiMyPe() == 0) {
    memset(received, 0, sizeof(received));
    issue_all();
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
