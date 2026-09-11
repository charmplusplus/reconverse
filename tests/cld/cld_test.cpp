// The seed-balancer (Cld) API at the Converse level: CldEnqueue to a PE,
// to CLD_ANYWHERE, CLD_BROADCAST_ALL; CldNodeEnqueue; CldEnqueueMulti;
// CldEnqueueGroup; CldEnqueueWithinNode; with and without a pack function.
// Charm++ routes every chare creation through these; reconverse's suite did
// not call them. PE 0 issues each kind once with the expected delivery
// count, every delivery acks to PE 0, and PE 0 exits everyone when the
// counts match. A delivery beyond its count aborts.
#include "converse.h"
#include <atomic>
#include <cstdio>
#include <cstring>

enum Kind {
  K_PE,
  K_ANY,
  K_BCAST_ALL,
  K_NODE,
  K_NODE_ANY,
  K_MULTI,
  K_GROUP,
  K_WITHIN,
  K_PACKED,
  K_COUNT
};
static const char *kindName[K_COUNT] = {"CldEnqueue(pe)",
                                        "CldEnqueue(CLD_ANYWHERE)",
                                        "CldEnqueue(CLD_BROADCAST_ALL)",
                                        "CldNodeEnqueue(node)",
                                        "CldNodeEnqueue(CLD_ANYWHERE)",
                                        "CldEnqueueMulti",
                                        "CldEnqueueGroup",
                                        "CldEnqueueWithinNode",
                                        "CldEnqueue(pe) with pack fn"};

struct Msg {
  CmiMessageHeader header;
  int kind;
  int packed;
  int payload[4];
};

CpvDeclare(int, workIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);
CpvDeclare(int, infoIdx);
CpvDeclare(int, infoPackIdx);
CpvDeclare(int, packIdx);
static int expected[K_COUNT];
static int received[K_COUNT];
static int remaining;

static void info_fn(void *vmsg, CldPackFn *pfn, int *len, int *queueing,
                    int *priobits, unsigned int **prioptr) {
  static unsigned int prio = 0;
  *pfn = NULL;
  *len = sizeof(Msg);
  *queueing = CQS_QUEUEING_FIFO;
  *priobits = 0;
  *prioptr = &prio;
}

static void pack_fn(void *vmsgptr) {
  // Cld calls the pack function with the address of the message pointer.
  Msg *m = *(Msg **)vmsgptr;
  m->packed = 1;
}

static void info_pack_fn(void *vmsg, CldPackFn *pfn, int *len, int *queueing,
                         int *priobits, unsigned int **prioptr) {
  info_fn(vmsg, pfn, len, queueing, priobits, prioptr);
  *pfn = pack_fn;
}

static Msg *make(int kind) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  CmiSetHandler(m, CpvAccess(workIdx));
  m->kind = kind;
  m->packed = 0;
  for (int i = 0; i < 4; i++)
    m->payload[i] = 100 * kind + i;
  return m;
}

static void work_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  for (int i = 0; i < 4; i++)
    if (m->payload[i] != 100 * m->kind + i)
      CmiAbort("cld test: payload corrupted for %s", kindName[m->kind]);
  if (m->kind == K_PACKED && !m->packed && CmiNodeOf(0) != CmiMyNode())
    CmiAbort("cld test: message to a remote node was not packed");
  m->header.handlerId = CpvAccess(ackIdx);
  CmiSyncSendAndFree(0, sizeof(Msg), m);
}

static void ack_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  int k = m->kind;
  CmiFree(m);
  received[k]++;
  if (received[k] > expected[k])
    CmiAbort("cld test: %s delivered %d times, expected %d", kindName[k],
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
  int info = CpvAccess(infoIdx), infoPack = CpvAccess(infoPackIdx);

  expected[K_PE] = 1;
  CldEnqueue(npes - 1, make(K_PE), info);

  expected[K_ANY] = 5;
  for (int i = 0; i < 5; i++)
    CldEnqueue(CLD_ANYWHERE, make(K_ANY), info);

  expected[K_BCAST_ALL] = npes;
  CldEnqueue(CLD_BROADCAST_ALL, make(K_BCAST_ALL), info);

  expected[K_NODE] = 1;
  CldNodeEnqueue(nnodes - 1, make(K_NODE), info);

  expected[K_NODE_ANY] = 3;
  for (int i = 0; i < 3; i++)
    CldNodeEnqueue(CLD_ANYWHERE, make(K_NODE_ANY), info);

  int *pes = new int[npes];
  for (int i = 0; i < npes; i++)
    pes[i] = i;
  expected[K_MULTI] = npes;
  CldEnqueueMulti(npes, pes, make(K_MULTI), info);

  expected[K_GROUP] = npes;
  CmiGroup grp = CmiEstablishGroup(npes, pes);
  CldEnqueueGroup(grp, make(K_GROUP), info);
  delete[] pes;

  expected[K_WITHIN] = CmiMyNodeSize();
  CldEnqueueWithinNode(make(K_WITHIN), info);

  expected[K_PACKED] = 1;
  CldEnqueue(npes - 1, make(K_PACKED), infoPack);

  remaining = K_COUNT;
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, workIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvInitialize(int, infoIdx);
  CpvInitialize(int, infoPackIdx);
  CpvInitialize(int, packIdx);
  CpvAccess(workIdx) = CmiRegisterHandler(work_handler);
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  CpvAccess(infoIdx) = CldRegisterInfoFn(info_fn);
  CpvAccess(infoPackIdx) = CldRegisterInfoFn(info_pack_fn);
  CpvAccess(packIdx) = CldRegisterPackFn(pack_fn);
  if (CmiMyPe() == 0) {
    CmiPrintf("[0] seed balancer strategy: %s\n", CldGetStrategy());
    memset(received, 0, sizeof(received));
    issue_all();
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
