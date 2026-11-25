/***************************************************************
  CmiPut ping-pong between two PEs, for the cost of a large put from
  memory the network has not registered (CmiPut takes a plain source
  pointer, so the backend registers it per put, unless a registration
  cache keeps it).

  PE 0 and the first PE of the next process (or PE 1 in one process) each
  own a region and a counter that re-arms itself. A puts S bytes into B's
  region; B's counter action puts S bytes back into A's; A times the round
  trips. Source buffers:
    fixed  one buffer per PE, the same every put (a registration cache hits)
    fresh  a buffer malloc'd for each put and freed once its put's source
           completion has run (big ones are new mappings, so a cache misses)
  Run with LCI_ATTR_USE_REG_CACHE=0 for registration on every put, and with
  +put_message_max_bytes <large> for the message path at the same sizes.

  usage: region_put_bench <fixed|fresh> [iters=N] [max=BYTES]
  prints one line per size: "region_put_bench <mode> <bytes> B: <us> us
  per round trip (min <us>)"
 ****************************************************************/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "converse.h"
#include "conv-region.h"

static bool g_fresh = false;
static int g_iters = 200;
static size_t g_max = 4u << 20;
static const int kWarmup = 20;

struct TargetMsg {
  char core[CmiMsgHeaderSizeBytes];
  CmiTarget t;
};

CpvDeclare(int, targetIdx);
CpvDeclare(int, startIdx);

struct PeState {
  CmiRegion region = nullptr;
  unsigned char *mem = nullptr;
  CmiCounter arrive = nullptr, sent = nullptr;
  CmiAttachment peer = nullptr;
  unsigned char *fixed = nullptr;
  unsigned char *pending = nullptr; // fresh: the buffer of the put in flight
  size_t size = 0;
  int iter = 0, total = 0;
  double t0 = 0, best = 1e30, sum = 0;
  size_t sizeIndex = 0;
  std::vector<size_t> sizes;
};
CpvDeclare(PeState *, st);

// PE 0's partner: the first PE of the next process, or PE 1
static int second() { return CmiNumNodes() > 1 ? CmiNodeFirst(1) : 1; }
static int partner() { return CmiMyPe() == 0 ? second() : 0; }

static void putOnce(PeState *s) {
  unsigned char *src = s->fixed;
  if (g_fresh) {
    src = (unsigned char *)malloc(s->size);
    s->pending = src;
  }
  src[0] = (unsigned char)s->iter;
  src[s->size - 1] = (unsigned char)(s->iter + 1);
  if (CmiPut(s->peer, 0, src, s->size, nullptr, s->sent, nullptr) != CMI_REGION_OK)
    CmiAbort("region_put_bench: CmiPut failed\n");
}

static void onSent(void *, CmiCounter) {
  PeState *s = CpvAccess(st);
  if (g_fresh && s->pending != nullptr) {
    free(s->pending);
    s->pending = nullptr;
  }
}

static void nextSize(PeState *s);

// The data has arrived: PE 0 times it and starts the next; the other
// answers.
static void onArrive(void *, CmiCounter) {
  PeState *s = CpvAccess(st);
  if (s->mem[0] != (unsigned char)s->iter ||
      s->mem[s->size - 1] != (unsigned char)(s->iter + 1))
    CmiAbort("region_put_bench: wrong data in round trip %d at %zu bytes\n",
             s->iter, s->size);
  if (CmiMyPe() != 0) {
    putOnce(s);
    s->iter++;
    return;
  }
  double now = CmiWallTimer();
  if (s->iter >= kWarmup) {
    double dt = now - s->t0;
    s->sum += dt;
    if (dt < s->best) s->best = dt;
  }
  s->iter++;
  if (s->iter == kWarmup + g_iters) {
    CmiPrintf("region_put_bench %s %zu B: %.2f us per round trip (min %.2f)\n",
              g_fresh ? "fresh" : "fixed", s->size, s->sum / g_iters * 1e6,
              s->best * 1e6);
    s->sizeIndex++;
    nextSize(s);
    return;
  }
  s->t0 = CmiWallTimer();
  putOnce(s);
}

struct StartMsg {
  char core[CmiMsgHeaderSizeBytes];
  size_t size;
};

// Both sides switch sizes before the first put of a size: PE 0 tells its
// partner, which answers before PE 0 puts.
static void startHandler(void *vmsg) {
  StartMsg *m = (StartMsg *)vmsg;
  PeState *s = CpvAccess(st);
  s->size = m->size;
  s->iter = 0;
  if (CmiMyPe() != 0) {
    CmiSyncSendAndFree(0, sizeof(StartMsg), m);
    return;
  }
  CmiFree(m);
  s->t0 = CmiWallTimer();
  putOnce(s);
}

static void nextSize(PeState *s) {
  if (s->sizeIndex == s->sizes.size()) {
    CmiPrintf("region_put_bench PASS\n");
    CmiExit(0);
    return;
  }
  s->size = s->sizes[s->sizeIndex];
  s->sum = 0;
  s->best = 1e30;
  StartMsg *m = (StartMsg *)CmiAlloc(sizeof(StartMsg));
  m->size = s->size;
  CmiSetHandler(m, CpvAccess(startIdx));
  CmiSyncSendAndFree(partner(), sizeof(StartMsg), m);
}

static void targetHandler(void *vmsg) {
  TargetMsg *m = (TargetMsg *)vmsg;
  PeState *s = CpvAccess(st);
  s->peer = CmiAttachTarget(&m->t);
  CmiFree(m);
  if (CmiMyPe() == 0) nextSize(s);
}

CmiStartFn mymain(int argc, char **argv) {
  (void)argc;
  (void)argv;
  CpvInitialize(int, targetIdx);
  CpvAccess(targetIdx) = CmiRegisterHandler(targetHandler);
  CpvInitialize(int, startIdx);
  CpvAccess(startIdx) = CmiRegisterHandler(startHandler);
  CpvInitialize(PeState *, st);
  PeState *s = CpvAccess(st) = new PeState();
  if (CmiNumPes() < 2) CmiAbort("region_put_bench: needs two PEs\n");
  if (CmiMyPe() != 0 && CmiMyPe() != second()) return 0;
  for (size_t n = 16384; n <= g_max; n *= 4) s->sizes.push_back(n);
  s->mem = (unsigned char *)CmiRegionAlloc(g_max, CMI_MEM_HOST, 0, 0, &s->region);
  s->fixed = (unsigned char *)malloc(g_max);
  memset(s->fixed, 1, g_max);
  s->arrive = CmiCounterCreate(1, CmiCounterActionFn(onArrive, nullptr),
                               CMI_COUNTER_AUTO_REARM);
  s->sent = CmiCounterCreate(1, CmiCounterActionFn(onSent, nullptr),
                             CMI_COUNTER_AUTO_REARM);
  TargetMsg *m = (TargetMsg *)CmiAlloc(sizeof(TargetMsg));
  m->t = CmiTargetMake(s->region, CMI_PIECE_ALL, s->arrive);
  CmiSetHandler(m, CpvAccess(targetIdx));
  CmiSyncSendAndFree(partner(), sizeof(TargetMsg), m);
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "fresh") == 0) g_fresh = true;
    else if (strncmp(argv[i], "iters=", 6) == 0) g_iters = atoi(argv[i] + 6);
    else if (strncmp(argv[i], "max=", 4) == 0) g_max = (size_t)atoll(argv[i] + 4);
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
