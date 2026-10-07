// Does node-queue traffic slow down unrelated PEs through a shared cache line?
//
// CsdNodeQueueLen is written on every CsdNodeEnqueueGeneral and every
// node-queue dequeue. If it shares a cache line with globals that other PEs
// read on their hot path -- Cmi_startTime, read by every CmiWallTimer() --
// each write invalidates that line on every core.
//
// Single process. The first -timers PEs (default: half) each run one handler
// that calls CmiWallTimer() in a loop for -seconds and counts the calls. The
// remaining PEs keep -inflight messages cycling through the priority node
// queue: each message's handler re-enqueues it with CsdNodeEnqueueGeneral.
// With -idle they sit in the scheduler with no node-queue traffic instead,
// which is the control: same timer PEs, same number of spinning PEs. PE 0 (a
// timer PE) reports the median and minimum call rate over the timer PEs and
// the node-queue message rate.
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include "converse.h"

struct Msg {
  char header[CmiMsgHeaderSizeBytes];
  double value;
};

static int cycle_idx, timer_idx, result_idx;
static int ntimers = -1;
static int idle = 0;
static int inflight = 0;
static double seconds = 2.0;
static std::atomic<int> stop_flag{0};
static std::atomic<long> cycled{0};
CpvDeclare(std::vector<double> *, rates);

static int firstProducer() { return ntimers; }

static void cycleHandler(void *vmsg) {
  if (stop_flag.load(std::memory_order_relaxed)) {
    CmiFree(vmsg);
    return;
  }
  cycled.fetch_add(1, std::memory_order_relaxed);
  CsdNodeEnqueueGeneral(vmsg, CQS_QUEUEING_FIFO, 0, NULL);
}

static void timerHandler(void *vmsg) {
  CmiFree(vmsg);
  double start = CmiWallTimer(), now = start;
  long calls = 0;
  while (now - start < seconds) {
    now = CmiWallTimer();
    calls++;
  }
  Msg *r = (Msg *)CmiAlloc(sizeof(Msg));
  r->value = calls / (now - start) / 1e6; // calls per microsecond
  CmiSetHandler(r, result_idx);
  CmiSyncSendAndFree(0, sizeof(Msg), r);
}

static double t_begin;
static long cycled_begin;

static void resultHandler(void *vmsg) {
  std::vector<double> &v = *CpvAccess(rates);
  v.push_back(((Msg *)vmsg)->value);
  CmiFree(vmsg);
  if ((int)v.size() < firstProducer()) return;
  double elapsed = CmiWallTimer() - t_begin;
  long n = cycled.load() - cycled_begin;
  stop_flag.store(1);
  std::sort(v.begin(), v.end());
  CmiPrintf("nodeq_sharing: PEs=%d timer_PEs=%d producers=%d inflight=%d "
            "CmiWallTimer calls/us per timer PE: median %.2f min %.2f max %.2f; "
            "node-queue msgs/us %.2f\n",
            CmiMyNodeSize(), (int)v.size(), idle ? 0 : CmiMyNodeSize() - ntimers,
            idle ? 0 : inflight,
            v[v.size() / 2], v.front(), v.back(), n / elapsed / 1e6);
  CmiExit(0);
}

static void moduleInit(int argc, char **argv) {
  CpvInitialize(std::vector<double> *, rates);
  CpvAccess(rates) = new std::vector<double>();
  cycle_idx = CmiRegisterHandler(cycleHandler);
  timer_idx = CmiRegisterHandler(timerHandler);
  result_idx = CmiRegisterHandler(resultHandler);
  CmiGetArgInt(argv, "-timers", &ntimers);
  idle = CmiGetArgFlag(argv, "-idle");
  CmiGetArgInt(argv, "-inflight", &inflight);
  CmiGetArgDouble(argv, "-seconds", &seconds);
  if (ntimers < 0) ntimers = CmiMyNodeSize() / 2;
  if (inflight <= 0) inflight = 4 * std::max(CmiMyNodeSize() - ntimers, 1);
  if (CmiNumNodes() != 1 || ntimers < 1 || ntimers >= CmiMyNodeSize()) {
    if (CmiMyPe() == 0) {
      CmiPrintf("nodeq_sharing: needs 1 process and 1 <= -timers < PEs\n");
      CmiExit(1);
    }
    return;
  }
  CmiNodeAllBarrier();
  if (CmiMyRank() == 0) {
    t_begin = CmiWallTimer();
    cycled_begin = cycled.load();
    if (!idle)
      for (int i = 0; i < inflight; i++) {
        Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
        CmiSetHandler(m, cycle_idx);
        CsdNodeEnqueueGeneral(m, CQS_QUEUEING_FIFO, 0, NULL);
      }
  }
  // Timer PEs start their loop from the scheduler, so producers that are
  // still in moduleInit do not matter; the timer handler never returns to the
  // scheduler until it is done, so timer PEs do not consume node messages.
  if (CmiMyRank() < firstProducer()) {
    Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
    CmiSetHandler(m, timer_idx);
    CmiSyncSendAndFree(CmiMyPe(), sizeof(Msg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, moduleInit, 0, 0);
  return 0;
}
