#include "converse.h"
#include <atomic>
#include <cstdint>
#include <cstdio>

// Checks the delivery contracts of CmiWithinNodeBroadcast, which Charm++'s
// CkBroadcastWithinNode and its node-level broadcast forwarding depend on
// (tests/charm++/within_node_bcast, tests/charm++/zerocopy):
//   phase 0: an ordinary message is copied for every PE on the node, so each
//            PE sees a distinct buffer;
//   phase 1: a message flagged nokeep is shared, so every PE sees the same
//            buffer, holding one reference each;
//   phase 2: a zerocopy broadcast receive message (CMK_ZC_BCAST_RECV_MSG) is
//            delivered to the calling PE only; the zerocopy protocol forwards
//            it to the peers itself, later;
//   phase 3: an ordinary message telling every PE to exit. It is sent after
//            phase 2 by the same PE, so a phase-2 copy wrongly forwarded to a
//            peer is queued ahead of it and is caught before that PE exits.
// Rank 0 of each node starts phase 0; the last PE to receive a phase starts
// the next one. No PE frees a message before the phase it belongs to has
// been verified: a freed buffer's address can be handed out again for a
// later copy, which would make "distinct" and "identical" checks
// meaningless. Phase-0 copies are freed by their receiving PEs during phase
// 3; the phase-1 buffer's references are all dropped by the verifying PE.
// With one PE per node the checks are trivially true, so run with +pe >= 2.

static const int kMaxNodeSize = 1024;

struct Message {
  CmiMessageHeader header;
  int phase;
};

CpvDeclare(int, handlerId);
static std::atomic<int> arrivals[4];
static std::atomic<uintptr_t> seen[2][kMaxNodeSize];
static std::atomic<int> zcSender{-1};

static Message *make(int phase, bool nokeep, bool zcRecv) {
  Message *msg = (Message *)CmiAlloc(sizeof(Message));
  msg->header.handlerId = CpvAccess(handlerId);
  msg->header.messageSize = sizeof(Message);
  msg->header.nokeep = nokeep;
  if (zcRecv)
    msg->header.zcMsgType = CMK_ZC_BCAST_RECV_MSG;
  msg->phase = phase;
  return msg;
}

static void check(int phase) {
  int n = CmiMyNodeSize();
  for (int i = 0; i < n; i++) {
    uintptr_t p = seen[phase][i].load();
    if (p == 0)
      CmiAbort("within-node-bcast: phase %d, rank %d received nothing", phase,
               i);
    for (int j = 0; j < i; j++) {
      uintptr_t q = seen[phase][j].load();
      if (phase == 0 && p == q)
        CmiAbort("within-node-bcast: copy broadcast delivered one buffer %p "
                 "to ranks %d and %d",
                 (void *)p, j, i);
      if (phase == 1 && p != q)
        CmiAbort("within-node-bcast: nokeep broadcast delivered distinct "
                 "buffers %p (rank %d) and %p (rank %d)",
                 (void *)q, j, (void *)p, i);
    }
  }
  if (phase == 1) {
    void *shared = (void *)seen[1][0].load();
    int refs = CmiGetReference(shared);
    if (refs != n)
      CmiAbort("within-node-bcast: nokeep buffer holds %d references, "
               "expected one per PE (%d)",
               refs, n);
    for (int i = 0; i < n; i++)
      CmiFree(shared);
  }
  CmiPrintf("[%d] phase %d (%s) verified on %d PEs\n", CmiMyPe(), phase,
            phase == 0 ? "copies" : "shared nokeep", n);
}

static void handler(void *vmsg) {
  Message *msg = (Message *)vmsg;
  int phase = msg->phase;
  int before = -1;
  if (phase < 2) {
    seen[phase][CmiMyRank()].store((uintptr_t)vmsg);
    before = arrivals[phase].fetch_add(1);
  }
  switch (phase) {
  case 0:
  case 1:
    if (before == CmiMyNodeSize() - 1) {
      check(phase);
      if (phase == 0) {
        Message *next = make(1, true, false);
        CmiWithinNodeBroadcast(next->header.messageSize, next);
      } else {
        zcSender.store(CmiMyRank());
        Message *next = make(2, false, true);
        CmiWithinNodeBroadcast(next->header.messageSize, next);
      }
    }
    break;
  case 2:
    if (CmiMyRank() != zcSender.load())
      CmiAbort("within-node-bcast: zerocopy receive broadcast from rank %d "
               "was delivered to rank %d; it must reach the caller only",
               zcSender.load(), CmiMyRank());
    if (arrivals[2].fetch_add(1) != 0)
      CmiAbort("within-node-bcast: zerocopy receive broadcast delivered "
               "more than once to rank %d",
               CmiMyRank());
    CmiFree(vmsg);
    CmiPrintf("[%d] phase 2 (zerocopy receive, caller only) verified\n",
              CmiMyPe());
    {
      Message *next = make(3, false, false);
      CmiWithinNodeBroadcast(next->header.messageSize, next);
    }
    break;
  case 3:
    CmiFree(vmsg);
    // Phase 0 was verified long ago; release this PE's own copy from it.
    CmiFree((void *)seen[0][CmiMyRank()].load());
    CsdExitScheduler();
    break;
  default:
    CmiAbort("within-node-bcast: unexpected phase %d", phase);
  }
}

CmiStartFn mymain(int argc, char **argv) {
  CpvInitialize(int, handlerId);
  CpvAccess(handlerId) = CmiRegisterHandler(handler);
  if (CmiMyNodeSize() > kMaxNodeSize)
    CmiAbort("within-node-bcast: node size %d exceeds test limit %d",
             CmiMyNodeSize(), kMaxNodeSize);
  if (CmiMyRank() == 0) {
    Message *msg = make(0, false, false);
    CmiWithinNodeBroadcast(msg->header.messageSize, msg);
  }
  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
