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
           completion has run (malloc may hand back the same addresses
           every time, as glibc does once it has freed a mapping of that
           size, and then a cache hits as with fixed)
    rotate count=K (default 64) buffers of max bytes, allocated and touched
           at start; each put uses the next in turn, so a cache hits only
           if it holds K registrations (or K * size bytes). Each size's
           first max(20, K) round trips are warm-up, not timed, so every
           buffer has been put from once at that size before timing.
    mmap   each put's source a new anonymous mmap of the put's size, filled
           before the put and munmapped once its source completion has run:
           new pages every put, even if mmap reuses the address, so a cache
           that sees the munmap must register again. PE 0 also times its
           own mmap, fill (page faults and writes) and munmap per put; a
           round trip holds two of each, one on either side.
  Run with LCI_ATTR_USE_REG_CACHE=0 (ibv) or FI_MR_CACHE_MAX_COUNT=0 (OFI)
  for registration on every put, and with +put_message_max_bytes <large>
  for the message path at the same sizes.

  usage: region_put_bench <fixed|fresh|rotate|mmap> [count=K] [iters=N]
         [max=BYTES]
  prints one line per size: "region_put_bench <mode> <bytes> B: <us> us
  per round trip (min <us>)", the mode as rotate<K> for rotate; fresh and
  mmap add how many timed puts had a source address other than the last
  put's, and mmap the times of its mmap, fill and munmap
 ****************************************************************/

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "converse.h"
#include "conv-region.h"

enum Mode { kFixed, kFresh, kRotate, kMmap };
static Mode g_mode = kFixed;
static int g_count = 64; // rotate: how many buffers
static int g_iters = 200;
static size_t g_max = 4u << 20;
static const int kWarmup = 20;
static int g_warmup = kWarmup; // rotate: max(kWarmup, g_count)
static char g_name[32];

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
  std::vector<unsigned char *> bufs; // rotate
  size_t next = 0;
  // fresh, mmap: the buffers of puts whose source completion has not run
  struct Pending {
    unsigned char *p;
    size_t len;
    bool timed;
  };
  std::deque<Pending> pending;
  // PE 0, over the timed puts of a size
  int timedPuts = 0, newAddr = 0, overlaps = 0;
  const unsigned char *lastSrc = nullptr;
  double mapSum = 0, fillSum = 0, unmapSum = 0;
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
  // PE 0 starts round trip s->iter here; the others answer with theirs
  bool timed = CmiMyPe() == 0 && s->iter >= g_warmup;
  unsigned char *src = s->fixed;
  if (!s->pending.empty() && timed) s->overlaps++;
  if (g_mode == kFresh) {
    src = (unsigned char *)malloc(s->size);
    s->pending.push_back({src, s->size, timed});
  } else if (g_mode == kRotate) {
    src = s->bufs[s->next];
    s->next = (s->next + 1) % s->bufs.size();
  } else if (g_mode == kMmap) {
#ifndef _WIN32
    double t0 = CmiWallTimer();
    void *p = mmap(nullptr, s->size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) CmiAbort("region_put_bench: mmap failed\n");
    double t1 = CmiWallTimer();
    memset(p, (unsigned char)(s->iter + 7), s->size);
    double t2 = CmiWallTimer();
    if (timed) {
      s->mapSum += t1 - t0;
      s->fillSum += t2 - t1;
    }
    src = (unsigned char *)p;
    s->pending.push_back({src, s->size, timed});
#endif
  }
  if (timed) {
    s->timedPuts++;
    if (src != s->lastSrc) s->newAddr++;
  }
  s->lastSrc = src;
  src[0] = (unsigned char)s->iter;
  src[s->size - 1] = (unsigned char)(s->iter + 1);
  if (CmiPut(s->peer, 0, src, s->size, nullptr, s->sent, nullptr) != CMI_REGION_OK)
    CmiAbort("region_put_bench: CmiPut failed\n");
}

// A put's source completion releases the oldest buffer in flight. One put
// is in flight at a time, unless a completion runs after its round trip has
// ended; the line reports how often that happened.
static void onSent(void *, CmiCounter) {
  PeState *s = CpvAccess(st);
  if (s->pending.empty()) return;
  PeState::Pending b = s->pending.front();
  s->pending.pop_front();
  if (g_mode == kFresh) {
    free(b.p);
  } else if (g_mode == kMmap) {
#ifndef _WIN32
    double t0 = CmiWallTimer();
    munmap(b.p, b.len);
    if (b.timed) s->unmapSum += CmiWallTimer() - t0;
#endif
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
  if (s->iter >= g_warmup) {
    double dt = now - s->t0;
    s->sum += dt;
    if (dt < s->best) s->best = dt;
  }
  s->iter++;
  if (s->iter == g_warmup + g_iters) {
    char extra[256] = "";
    int n = s->timedPuts > 0 ? s->timedPuts : 1;
    if (g_mode == kFresh || g_mode == kMmap)
      snprintf(extra, sizeof extra, "; new source address in %d of %d puts",
               s->newAddr, s->timedPuts);
    if (g_mode == kMmap)
      snprintf(extra + strlen(extra), sizeof extra - strlen(extra),
               "; per put on PE 0: mmap %.2f us, fill %.2f us, munmap %.2f us",
               s->mapSum / n * 1e6, s->fillSum / n * 1e6,
               s->unmapSum / n * 1e6);
    if (s->overlaps > 0)
      snprintf(extra + strlen(extra), sizeof extra - strlen(extra),
               "; %d puts began before the last one's source completion",
               s->overlaps);
    CmiPrintf("region_put_bench %s %zu B: %.2f us per round trip (min %.2f)%s\n",
              g_name, s->size, s->sum / g_iters * 1e6, s->best * 1e6, extra);
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
  s->timedPuts = s->newAddr = s->overlaps = 0;
  s->mapSum = s->fillSum = s->unmapSum = 0;
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
  // Bind this PE per +setcpuaffinity/+pemap (every PE takes part);
  // ConverseInit does not.
  CmiInitCPUAffinity(argv);
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
  if (g_mode == kRotate)
    for (int i = 0; i < g_count; i++) {
      s->bufs.push_back((unsigned char *)malloc(g_max));
      memset(s->bufs.back(), 1, g_max);
    }
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
    if (strcmp(argv[i], "fresh") == 0) g_mode = kFresh;
    else if (strcmp(argv[i], "rotate") == 0) g_mode = kRotate;
    else if (strcmp(argv[i], "mmap") == 0) g_mode = kMmap;
    else if (strncmp(argv[i], "count=", 6) == 0) g_count = atoi(argv[i] + 6);
    else if (strncmp(argv[i], "iters=", 6) == 0) g_iters = atoi(argv[i] + 6);
    else if (strncmp(argv[i], "max=", 4) == 0) g_max = (size_t)atoll(argv[i] + 4);
  }
#ifdef _WIN32
  if (g_mode == kMmap) {
    fprintf(stderr, "region_put_bench: no mmap mode on Windows\n");
    return 1;
  }
#endif
  if (g_count < 1) g_count = 1;
  static const char *names[] = {"fixed", "fresh", "rotate", "mmap"};
  if (g_mode == kRotate) {
    g_warmup = std::max(kWarmup, g_count);
    snprintf(g_name, sizeof g_name, "rotate%d", g_count);
  } else {
    snprintf(g_name, sizeof g_name, "%s", names[g_mode]);
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
