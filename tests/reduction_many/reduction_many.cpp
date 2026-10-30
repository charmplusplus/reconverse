// Many reductions over the life of a program, several in flight at once.
//
// The reduction table has CmiMaxReductions (16) slots. Each reduction id
// takes slot id mod 16, so a program can run any number of reductions as
// long as no more than 16 are unfinished at a time. This test runs `rounds`
// rounds; in each, every contributor starts `window` reductions back to back,
// and the next round starts once the root has all of their results. Every
// result is checked, and the merge function aborts if contributions from two
// different reductions are ever combined.
//
// Mode `pe` uses CmiReduce (every PE contributes), mode `node` uses
// CmiNodeReduce (one PE per process contributes). A window above 16 must
// abort with "Too many simultaneous reductions".
//
// Usage: reduction_many +pe N [pe|node] [rounds] [window]
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

struct Message {
  CmiMessageHeader header;
  int round, slot;  // which reduction this contribution belongs to
  long long value;
};

static int done_idx, start_idx;
static bool nodeMode = false;
static int rounds = 50, window = 8;
// On the root only. A node reduction's result is handled by whichever PE of
// node 0 merges its last contribution (a node send, as in classic Converse),
// so with several PEs per process two results can be handled at once.
static std::atomic<int> doneInRound{0};

static int contributors() { return nodeMode ? CmiNumNodes() : CmiNumPes(); }
static int me() { return nodeMode ? CmiMyNode() : CmiMyPe(); }

// every contributor adds its own index plus the reduction's sequence number
static long long expected(int round, int slot) {
  long long n = contributors();
  return n * (n - 1) / 2 + n * (long long)(round * window + slot);
}

static void *merge(int *size, void *local, void **remote, int n) {
  Message *l = (Message *)local;
  for (int i = 0; i < n; i++) {
    Message *r = (Message *)remote[i];
    if (r->round != l->round || r->slot != l->slot)
      CmiAbort("reduction_many: merged round %d slot %d into round %d slot %d",
               r->round, r->slot, l->round, l->slot);
    l->value += r->value;  // the runtime frees r after the merge
  }
  return l;
}

static void contribute(int round) {
  for (int s = 0; s < window; s++) {
    Message *m = (Message *)CmiAlloc(sizeof(Message));
    CmiSetHandler(m, done_idx);
    m->round = round;
    m->slot = s;
    m->value = me() + (long long)(round * window + s);
    if (nodeMode) CmiNodeReduce(m, sizeof(Message), merge);
    else CmiReduce(m, sizeof(Message), merge);
  }
}

static void startHandler(void *vmsg) {
  int round = ((Message *)vmsg)->round;
  CmiFree(vmsg);
  if (!nodeMode || CmiMyRank() == 0) contribute(round);
}

// runs on the root once a reduction completes: PE 0 in mode `pe`, any PE of
// node 0 in mode `node`
static void doneHandler(void *vmsg) {
  Message *m = (Message *)vmsg;
  if (m->value != expected(m->round, m->slot))
    CmiAbort("reduction_many: round %d slot %d: expected %lld, got %lld",
             m->round, m->slot, expected(m->round, m->slot), m->value);
  int round = m->round;
  CmiFree(m);
  // only the PE that counts the round's last result goes on; it resets the
  // count before starting the next round, whose results all come after
  if (doneInRound.fetch_add(1) + 1 < window) return;
  doneInRound = 0;
  if (round + 1 == rounds) {
    printf("reduction_many: %s, %d reductions (%d rounds of %d in flight) "
           "over %d contributors all correct\n",
           nodeMode ? "node" : "pe", rounds * window, rounds, window,
           contributors());
    CmiExit(0);
    return;
  }
  Message *next = (Message *)CmiAlloc(sizeof(Message));
  CmiSetHandler(next, start_idx);
  next->round = round + 1;
  CmiSyncBroadcastAllAndFree(sizeof(Message), next);
}

static void mymain(int argc, char **argv) {
  done_idx = CmiRegisterHandler(doneHandler);
  start_idx = CmiRegisterHandler(startHandler);
  if (argc > 1) nodeMode = strcmp(argv[1], "node") == 0;
  if (argc > 2) rounds = atoi(argv[2]);
  if (argc > 3) window = atoi(argv[3]);
  if (!nodeMode || CmiMyRank() == 0) contribute(0);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
