// The scheduler queue (src/queueing.cpp) and CqsEnqueueGeneral.
//
// Contract, the one the Charm++ queueing strategies document: messages
// come out in increasing priority value (negative before zero before
// positive); within one priority value, FIFO for QueuePush and the FIFO
// strategies, LIFO for QueuePushFront and the LIFO strategies, and the two
// mix at one level. Part 1 drives the Queue API on a private queue, part 2
// enqueues into the scheduler queue through CsdEnqueueGeneral with every
// integer strategy and records the order in which the handlers run.
#include "converse.h"
#include <cstdio>
#include <vector>

struct Msg {
  CmiMessageHeader header;
  int tag;
};

CpvDeclare(int, recordIdx);
CpvDeclare(int, exitIdx);
CpvDeclare(std::vector<int> *, order);

static const int kSched = 8;
// tag = enqueue position; strategy and priority per tag; expected run order
static const int schedStrategy[kSched] = {
    CQS_QUEUEING_IFIFO, CQS_QUEUEING_IFIFO, CQS_QUEUEING_FIFO,
    CQS_QUEUEING_ILIFO, CQS_QUEUEING_IFIFO, CQS_QUEUEING_LIFO,
    CQS_QUEUEING_IFIFO, CQS_QUEUEING_LLIFO};
static const long long schedPrio[kSched] = {3, -2, 0, 3, -2, 0, 7, -9};
// level -9: 7 | level -2: 1,4 | level 0: 5 (LIFO) then 2 | level 3: 3 (LIFO)
// then 0 | level 7: 6
static const int schedExpected[kSched] = {7, 1, 4, 5, 2, 3, 0, 6};

static Msg *make(int tag) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  m->tag = tag;
  return m;
}

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("queue test: %s", what);
}

static void expect_order(Queue q, const int *expected, int n,
                         const char *what) {
  for (int i = 0; i < n; i++) {
    Msg *m = (Msg *)QueueTop(q);
    check(m != NULL, "top NULL while not empty");
    if (m->tag != expected[i])
      CmiAbort("queue test: %s: pop %d gave tag %d, expected tag %d", what, i,
               m->tag, expected[i]);
    QueuePop(q);
    CmiFree(m);
  }
}

static void part1_direct(void) {
  QueueImpl impl;
  Queue q = &impl;
  QueueInit(q);
  check(QueueEmpty(q) == 1 && QueueSize(q) == 0, "fresh queue not empty");
  check(QueueTop(q) == NULL, "top of empty queue not NULL");
  QueuePop(q); // harmless on an empty queue

  // Order across priority values, FIFO within each.
  const long long prios[] = {2, 0, -1, 9, 0, -5, 0};
  const int n = sizeof(prios) / sizeof(prios[0]);
  for (int i = 0; i < n; i++) {
    QueuePush(q, make(i), prios[i]);
    check(QueueSize(q) == i + 1, "size after push");
  }
  check(QueueEmpty(q) == 0, "queue empty after pushes");
  const int expected[] = {5, 2, 1, 4, 6, 0, 3};
  expect_order(q, expected, n, "priority classes");
  check(QueueEmpty(q) == 1, "not empty after popping all");

  // FIFO within one nonzero priority, both signs, many entries.
  for (long long p : {5LL, -5LL}) {
    const int k = 50;
    int exp[k];
    for (int i = 0; i < k; i++) {
      QueuePush(q, make(i), p);
      exp[i] = i;
    }
    expect_order(q, exp, k, "FIFO within a level");
  }

  // LIFO within one level, and FIFO/LIFO mixed at one level: FIFO a, b;
  // LIFO c; FIFO d -> c, a, b, d. Same at priority zero.
  for (long long p : {4LL, 0LL, -4LL}) {
    QueuePush(q, make(0), p);
    QueuePush(q, make(1), p);
    QueuePushFront(q, make(2), p);
    QueuePush(q, make(3), p);
    const int exp[] = {2, 0, 1, 3};
    expect_order(q, exp, 4, "FIFO and LIFO mixed at one level");
  }
  {
    const int k = 20;
    int exp[k];
    for (int i = 0; i < k; i++) {
      QueuePushFront(q, make(i), 8);
      exp[i] = k - 1 - i;
    }
    expect_order(q, exp, k, "LIFO within a level");
  }

  // Many levels drained and reused: the kept-empty-level cache and the
  // erase path both run, and order stays right afterwards.
  for (int round = 0; round < 3; round++) {
    const int L = 30;
    int exp[L];
    for (int i = L - 1; i >= 0; i--)
      QueuePush(q, make(i), (long long)i * 3 - 40);
    for (int i = 0; i < L; i++)
      exp[i] = i;
    expect_order(q, exp, L, "levels drained and reused");
    check(QueueEmpty(q) == 1, "not empty after draining levels");
  }
  QueueDestroy(q);
  CmiPrintf("[%d] queue API: priority order, FIFO/LIFO within a level, "
            "level reuse ok\n",
            CmiMyPe());
}

static void record_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  std::vector<int> &order = *CpvAccess(order);
  order.push_back(m->tag);
  CmiFree(m);
  if ((int)order.size() == kSched) {
    for (int i = 0; i < kSched; i++)
      if (order[i] != schedExpected[i])
        CmiAbort("queue test: scheduler ran tag %d at position %d, expected "
                 "tag %d",
                 order[i], i, schedExpected[i]);
    CmiPrintf("[%d] CsdEnqueueGeneral FIFO/LIFO/IFIFO/ILIFO/LLIFO: scheduler "
              "order ok\n",
              CmiMyPe());
    CsdExitScheduler();
  }
}

static void part2_scheduler(void) {
  // All eight are queued before the scheduler starts, so the run order is
  // the queue order.
  for (int i = 0; i < kSched; i++) {
    Msg *m = make(i);
    CmiSetHandler(m, CpvAccess(recordIdx));
    long long lp = schedPrio[i];
    unsigned int ip = (unsigned int)(int)schedPrio[i];
    int st = schedStrategy[i];
    if (st == CQS_QUEUEING_LLIFO || st == CQS_QUEUEING_LFIFO)
      CsdEnqueueGeneral(m, st, 64, (unsigned int *)&lp);
    else
      CsdEnqueueGeneral(m, st, 32, &ip);
  }
}

static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, recordIdx);
  CpvInitialize(int, exitIdx);
  CpvInitialize(std::vector<int> *, order);
  CpvAccess(order) = new std::vector<int>();
  CpvAccess(recordIdx) = CmiRegisterHandler(record_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  part1_direct();
  if (CmiMyPe() == 0) {
    part2_scheduler();
  } else {
    Msg *m = make(0);
    CmiSetHandler(m, CpvAccess(exitIdx));
    CmiSyncSendAndFree(CmiMyPe(), sizeof(Msg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
