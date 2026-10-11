// Checks the order in which a PE's messages to itself are run.
//
// Each PE queues N messages to itself before its scheduler starts: the first
// N_FIFO through CmiSyncSend, the rest through CsdEnqueueGeneral with
// increasing integer priorities. Each carries a sequence number. Every
// message must run exactly once. Then, depending on argv[1]:
//   fifo    (default scheduler): each group runs in sequence order.
//   random  (+randomized_msgq):  each group's run order has more than N/10
//                                inversions.
// Each PE also prints a digest of its run order. With +randomized_seed the
// draws are fixed and every message is queued before the scheduler starts,
// so the digests of two runs must agree; repeat.cmake checks that.
// When every PE has finished, PE 0 broadcasts the exit.

#include <converse.h>
#include <cstdlib>
#include <cstring>
#include <vector>

static const int N = 2000;
static const int N_FIFO = 1500; // the rest go through CsdEnqueueGeneral

struct SeqMsg {
  char core[CmiMsgHeaderSizeBytes];
  int seq;
};

static bool expectRandom = false;

CpvDeclare(int, seqHandler);
CpvDeclare(int, doneHandler);
CpvDeclare(int, exitHandler);
CpvDeclare(std::vector<int> *, arrivals);
CpvDeclare(int, doneCount);

static long long countInversions(const std::vector<int> &v, int lo, int hi) {
  // inversions among entries whose value lies in [lo, hi)
  long long inv = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] < lo || v[i] >= hi) continue;
    for (size_t j = i + 1; j < v.size(); ++j)
      if (v[j] >= lo && v[j] < hi && v[j] < v[i]) ++inv;
  }
  return inv;
}

static void checkAndReport() {
  std::vector<int> &arr = *CpvAccess(arrivals);
  std::vector<int> seen(N, 0);
  for (int s : arr) {
    if (s < 0 || s >= N) CmiAbort("randomized_msgq: bad sequence number");
    seen[s]++;
  }
  for (int i = 0; i < N; ++i)
    if (seen[i] != 1) {
      CmiPrintf("[%d] sequence %d ran %d times\n", CmiMyPe(), i, seen[i]);
      CmiAbort("randomized_msgq: a message did not run exactly once");
    }

  unsigned long long digest = 1469598103934665603ULL; // FNV-1a over the order
  for (int s : arr) digest = (digest ^ (unsigned long long)s) * 1099511628211ULL;
  CmiPrintf("[%d] run order digest %016llx\n", CmiMyPe(), digest);

  long long all = countInversions(arr, 0, N);
  long long fifo = countInversions(arr, 0, N_FIFO);
  long long prio = countInversions(arr, N_FIFO, N);
  CmiPrintf("[%d] %s: %d messages ran once each; inversions: all %lld, "
            "CmiSyncSend group %lld, priority group %lld\n",
            CmiMyPe(), expectRandom ? "random" : "fifo", N, all, fifo, prio);
  if (expectRandom) {
    // Inversions are counted within each group: the default scheduler
    // already interleaves the two groups, so the total alone proves nothing.
    if (fifo <= N / 10 || prio <= N / 10)
      CmiAbort("randomized_msgq: run order is too close to send order");
  } else {
    if (fifo != 0)
      CmiAbort("randomized_msgq: CmiSyncSend messages ran out of order");
    if (prio != 0)
      CmiAbort("randomized_msgq: priority messages ran out of order");
  }

  char *done = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(done, CpvAccess(doneHandler));
  CmiSyncSendAndFree(0, CmiMsgHeaderSizeBytes, done);
}

static void handleSeq(SeqMsg *msg) {
  CpvAccess(arrivals)->push_back(msg->seq);
  CmiFree(msg);
  if ((int)CpvAccess(arrivals)->size() == N) checkAndReport();
}

static void handleDone(char *msg) {
  if (++CpvAccess(doneCount) == CmiNumPes()) {
    CmiPrintf("randomized_msgq: PASS\n");
    CmiSetHandler(msg, CpvAccess(exitHandler));
    CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, msg);
  } else {
    CmiFree(msg);
  }
}

static void handleExit(char *msg) {
  CmiFree(msg);
  CsdExitScheduler();
}

CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, seqHandler);
  CpvAccess(seqHandler) = CmiRegisterHandler((CmiHandler)handleSeq);
  CpvInitialize(int, doneHandler);
  CpvAccess(doneHandler) = CmiRegisterHandler((CmiHandler)handleDone);
  CpvInitialize(int, exitHandler);
  CpvAccess(exitHandler) = CmiRegisterHandler((CmiHandler)handleExit);
  CpvInitialize(std::vector<int> *, arrivals);
  CpvAccess(arrivals) = new std::vector<int>();
  CpvAccess(arrivals)->reserve(N);
  CpvInitialize(int, doneCount);
  CpvAccess(doneCount) = 0;

  argc = CmiGetArgc(argv);
  if (argc != 2 ||
      (strcmp(argv[1], "fifo") != 0 && strcmp(argv[1], "random") != 0))
    CmiAbort("Usage: ./randomized_msgq <fifo|random> [+randomized_msgq]");
  expectRandom = strcmp(argv[1], "random") == 0;

  // handlers are registered on every PE before anyone sends
  CmiNodeAllBarrier();

  for (int i = 0; i < N; ++i) {
    SeqMsg *msg = (SeqMsg *)CmiAlloc(sizeof(SeqMsg));
    msg->seq = i;
    CmiSetHandler(msg, CpvAccess(seqHandler));
    if (i < N_FIFO) {
      CmiSyncSendAndFree(CmiMyPe(), sizeof(SeqMsg), msg);
    } else {
      unsigned int prio = (unsigned int)i;
      CsdEnqueueGeneral(msg, CQS_QUEUEING_IFIFO, 8 * sizeof(int), &prio);
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
