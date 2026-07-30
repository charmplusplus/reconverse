/**
  Compares the scheduling rate of Converse active messages against the
  scheduling rate of C++20 coroutines.

  All three models are measured identically: -flows work items are kept in
  flight, a dispatch does nothing but hand control to the next item, and the
  reported number is elapsed / dispatches.

    coro      A coroutine handle is popped from a plain single-threaded ring
              FIFO, resumed to its next co_await, and pushed back. Nothing of
              the runtime is involved, so this is the floor: what a coroutine
              dispatch costs when the scheduler around it is as cheap as a
              scheduler can be.

    am        An active message is pushed onto this PE's queue with CmiPushPE,
              popped by CsdScheduler, and handed to its handler, which pushes it
              again. This is the runtime's real scheduling path -- the same
              queue and the same loop CthAwaken and ULT dispatch go through --
              so it carries the concurrent queue and the scheduler loop's
              per-iteration polling.

    coro+am   The two composed: every coroutine is driven by its own active
              message, so a dispatch is a queue pop, a handler call, and a
              resume. This is what coroutines would cost if this runtime
              scheduled them, and (coro+am - am) isolates what the resume adds
              on top of an active message.

  The gap between `coro` and `am` is therefore mostly the queue and scheduler
  loop, not the dispatch mechanism; `coro+am` is the number to use when asking
  whether coroutines are cheaper than what the runtime already does.

  Usage: reconverse_am_vs_coro +pe N [-flows K] [-events M] [-reps R]
  */
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(__cpp_impl_coroutine) || __cplusplus >= 202002L
#define HAVE_CORO 1
#include <coroutine>
#else
#define HAVE_CORO 0
#endif

#define DEFAULT_FLOWS 64
#define DEFAULT_EVENTS 1000000
#define DEFAULT_REPS 5

enum { PH_CORO = 0, PH_AM = 1, PH_CORO_AM = 2, PH_COUNT = 3 };
static const char *phaseName[PH_COUNT] = {"coro", "am", "coro+am"};

struct benchMsg {
  char header[CmiMsgHeaderSizeBytes];
  int index; /* which coroutine this message drives */
};

struct amBench {
  int flows;
  int nreps;
  long events;  /* target dispatches per rep */
  int phase;
  int rep;      /* -1 during each phase's warmup rep */
  long count;   /* dispatches so far this rep */
  long retired; /* messages that have stopped re-enqueueing */
  double start;
  double best[PH_COUNT], total[PH_COUNT];
  void **msgs;  /* `flows` reusable messages */
  void **coros; /* `flows` coroutine handle addresses */
};

CpvStaticDeclare(struct amBench, bench);
CpvStaticDeclare(int, amHandlerIdx);
CpvStaticDeclare(int, coroAmHandlerIdx);

static void startPhaseRep(void);

#if HAVE_CORO
struct Task {
  struct promise_type {
    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { abort(); }
  };
  std::coroutine_handle<promise_type> h;
};

/* Suspends at every step and never finishes: the driver resumes it a fixed
   number of times and destroys it afterwards, so a dispatch is exactly one
   resume and one suspend with no completion test to pay for. */
static Task stepCoro(void) {
  for (;;)
    co_await std::suspend_always{};
}

static inline void resumeCoro(void *addr) {
  std::coroutine_handle<>::from_address(addr).resume();
}
#else
static inline void resumeCoro(void *addr) { (void)addr; }
#endif

/* ------------------------------ reporting ------------------------------ */

static void recordRep(double elapsed, long dispatches) {
  struct amBench *d = &CpvAccess(bench);
  double usEach = 1.0e6 * elapsed / (double)dispatches;

  if (d->rep >= 0) { /* the warmup rep is measured but not recorded */
    d->total[d->phase] += usEach;
    if (usEach < d->best[d->phase])
      d->best[d->phase] = usEach;
    CmiPrintf("[PE %d] %-7s rep %d: %ld dispatches in %.6f s -> %.3f M/s, "
              "%.4f us each\n",
              CmiMyPe(), phaseName[d->phase], d->rep, dispatches, elapsed,
              1.0e-6 * dispatches / elapsed, usEach);
  }
}

static void report(void) {
  struct amBench *d = &CpvAccess(bench);

  for (int p = 0; p < PH_COUNT; p++)
    CmiPrintf("[PE %d] %-7s: best %.4f us/dispatch (%.3f M/s), mean %.4f us\n",
              CmiMyPe(), phaseName[p], d->best[p], 1.0 / d->best[p],
              d->total[p] / d->nreps);
  CmiPrintf("[PE %d] coroutine resume on top of an active message: %.4f us\n",
            CmiMyPe(), d->best[PH_CORO_AM] - d->best[PH_AM]);
  CmiPrintf("[PE %d] runtime scheduling on top of a bare resume: %.4f us\n",
            CmiMyPe(), d->best[PH_CORO_AM] - d->best[PH_CORO]);
  CmiPrintf("Format: DATA,am_vs_coro,{pe},{flows},{coro us},{am us},{coro+am "
            "us}\n");
  CmiPrintf("DATA,am_vs_coro,%d,%d,%f,%f,%f\n", CmiMyPe(), d->flows,
            d->best[PH_CORO], d->best[PH_AM], d->best[PH_CORO_AM]);
}

static void advance(void) {
  struct amBench *d = &CpvAccess(bench);

  d->rep++;
  if (d->rep < d->nreps) {
    startPhaseRep();
    return;
  }

  d->phase++;
  d->rep = -1;
  if (d->phase < PH_COUNT) {
    startPhaseRep();
    return;
  }

  report();
#if HAVE_CORO
  for (int i = 0; i < d->flows; i++)
    std::coroutine_handle<>::from_address(d->coros[i]).destroy();
#endif
  for (int i = 0; i < d->flows; i++)
    CmiFree(d->msgs[i]);
  free(d->msgs);
  free(d->coros);
  CsdExitScheduler();
}

/* --------------------- phase 1: bare coroutine loop --------------------- */

static void coroRep(void) {
  struct amBench *d = &CpvAccess(bench);

  /* a real FIFO, just the cheapest one that exists: a fixed ring with room
     for every handle plus the empty slot that separates head from tail */
  const int cap = d->flows + 1;
  void **q = (void **)malloc(cap * sizeof(void *));
  int head = 0, tail = 0;
  for (int i = 0; i < d->flows; i++)
    q[tail++] = d->coros[i];

  double t0 = CmiWallTimer();
  for (long n = 0; n < d->events; n++) {
    void *addr = q[head];
    head = (head + 1 == cap) ? 0 : head + 1;
    resumeCoro(addr);
    q[tail] = addr;
    tail = (tail + 1 == cap) ? 0 : tail + 1;
  }
  double elapsed = CmiWallTimer() - t0;

  free(q);
  recordRep(elapsed, d->events);
}

/* ------------------- phases 2 and 3: active messages ------------------- */

static void endAmRep(void) {
  struct amBench *d = &CpvAccess(bench);
  /* count, not events: the drain below lets a few extra dispatches through */
  recordRep(CmiWallTimer() - d->start, d->count);
  advance();
}

static void amHandler(void *msg) {
  struct amBench *d = &CpvAccess(bench);
  if (++d->count < d->events) {
    CmiPushPE(CmiMyRank(), msg);
    return;
  }
  if (++d->retired == d->flows) /* every message has come to rest */
    endAmRep();
}

static void coroAmHandler(void *msg) {
  struct amBench *d = &CpvAccess(bench);
  resumeCoro(d->coros[((struct benchMsg *)msg)->index]);
  if (++d->count < d->events) {
    CmiPushPE(CmiMyRank(), msg);
    return;
  }
  if (++d->retired == d->flows)
    endAmRep();
}

static void startAmRep(void) {
  struct amBench *d = &CpvAccess(bench);
  const int handler = (d->phase == PH_AM) ? CpvAccess(amHandlerIdx)
                                          : CpvAccess(coroAmHandlerIdx);
  d->count = 0;
  d->retired = 0;
  for (int i = 0; i < d->flows; i++)
    CmiSetHandler(d->msgs[i], handler);

  d->start = CmiWallTimer();
  for (int i = 0; i < d->flows; i++)
    CmiPushPE(CmiMyRank(), d->msgs[i]);
  /* the scheduler drains from here; endAmRep() picks it back up */
}

static void startPhaseRep(void) {
  if (CpvAccess(bench).phase == PH_CORO) {
    coroRep();
    advance();
  } else {
    startAmRep();
  }
}

/* -------------------------------- setup -------------------------------- */

static void bench_init(int argc, char **argv) {
  (void)argc;

  CpvInitialize(struct amBench, bench);
  CpvInitialize(int, amHandlerIdx);
  CpvInitialize(int, coroAmHandlerIdx);
  CpvAccess(amHandlerIdx) = CmiRegisterHandler((CmiHandler)amHandler);
  CpvAccess(coroAmHandlerIdx) = CmiRegisterHandler((CmiHandler)coroAmHandler);

  struct amBench *d = &CpvAccess(bench);
  d->flows = DEFAULT_FLOWS;
  d->nreps = DEFAULT_REPS;
  int events = DEFAULT_EVENTS;
  CmiGetArgInt(argv, "-flows", &d->flows);
  CmiGetArgInt(argv, "-events", &events);
  CmiGetArgInt(argv, "-reps", &d->nreps);
  d->events = events;

  if (d->flows < 1 || d->events < d->flows || d->nreps < 1) {
    if (CmiMyPe() == 0)
      CmiPrintf("Error: need -flows >= 1, -events >= flows and -reps >= 1 "
                "(got %d, %d, %d), exiting\n",
                d->flows, events, d->nreps);
    CmiExit(1); /* only queues the exit, so stop here ourselves */
    return;
  }

#if !HAVE_CORO
  if (CmiMyPe() == 0)
    CmiPrintf("note: built without C++20 coroutine support, the coro and "
              "coro+am phases measure an empty resume\n");
#endif

  d->phase = PH_CORO;
  d->rep = -1;
  d->count = d->retired = 0;
  for (int p = 0; p < PH_COUNT; p++) {
    d->best[p] = 1.0e30;
    d->total[p] = 0.0;
  }

  d->msgs = (void **)malloc(d->flows * sizeof(void *));
  d->coros = (void **)malloc(d->flows * sizeof(void *));
  for (int i = 0; i < d->flows; i++) {
    struct benchMsg *m = (struct benchMsg *)CmiAlloc(sizeof(struct benchMsg));
    m->index = i;
    /* CmiPushPE(rank, msg) reads the size back out of the header */
    ((CmiMessageHeader *)m)->messageSize = sizeof(struct benchMsg);
    d->msgs[i] = m;
#if HAVE_CORO
    d->coros[i] = stepCoro().h.address();
#else
    d->coros[i] = NULL;
#endif
  }

  if (CmiMyPe() == 0)
    CmiPrintf("Active message vs C++ coroutine scheduling: %d flows, "
              "%ld dispatches x %d reps (+1 warmup) per phase, on %d PEs\n",
              d->flows, d->events, d->nreps, CmiNumPes());

  /* skip the communication thread */
  if (CmiMyRank() != CmiMyNodeSize())
    startPhaseRep();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, bench_init);
  return 0;
}
