/**
  Verifies the correctness properties that am_vs_coro.cpp's `coro+am` phase
  assumes: that coroutines driven by the Converse scheduler actually interleave,
  and that each coroutine's frame survives across suspensions.

  -flows coroutines each take -steps steps. Every step appends (id, step) to a
  shared trace and then suspends; the active message that drove the resume is
  pushed back onto the PE queue, so the next dispatch belongs to whichever
  coroutine's message is next in the queue. At the end the trace is checked for:

    frame state   Each coroutine's steps appear exactly once and in increasing
                  order. `step` is a local of the coroutine body, so reading it
                  back correctly on the far side of a co_await is what proves
                  the frame persisted.

    interleaving  No two adjacent trace entries belong to the same coroutine
                  (with more than one flow). This is the direct evidence that
                  resume() returns and the scheduler keeps dispatching between
                  two steps of the same coroutine, rather than one coroutine
                  running to completion inside a single dispatch.

    fairness      The spread between the most- and least-advanced coroutine
                  never exceeds one step, i.e. the rotation is strict round
                  robin. This one is a property of the ready queue being FIFO
                  for a single producer, not of coroutines.

  It also reports whether the coroutine body ran on the same CthThread as the
  handler that resumed it -- it does, because a resume is an ordinary call that
  borrows the scheduler's stack rather than switching to one of its own.

  Usage: reconverse_coro_interleave +pe N [-flows F] [-steps S]
  */
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

#if !(defined(__cpp_impl_coroutine) || __cplusplus >= 202002L)
#error "coro_interleave requires C++20 coroutines"
#endif
#include <coroutine>

#define DEFAULT_FLOWS 4
#define DEFAULT_STEPS 5
#define MAX_PRINTED_TRACE 64

struct traceEntry {
  int id;
  int step;
};

struct benchMsg {
  char header[CmiMsgHeaderSizeBytes];
  int index;
};

struct interleaveState {
  int flows, steps;
  int traced;              /* trace entries recorded so far */
  int retired;             /* coroutines that have run to completion */
  struct traceEntry *trace;/* flows * steps entries */
  void **msgs;
  void **coros;
  CthThread handlerThread; /* CthSelf() seen by the handler */
  CthThread coroThread;    /* CthSelf() seen from inside a coroutine body */
};

CpvStaticDeclare(struct interleaveState, st);
CpvStaticDeclare(int, coroHandlerIdx);

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

static Task stepCoro(int id) {
  struct interleaveState *s = &CpvAccess(st);
  for (int step = 0; step < s->steps; step++) {
    /* `step` lives in the coroutine frame, so reading it back after the
       co_await below is what demonstrates the frame survived */
    s->trace[s->traced].id = id;
    s->trace[s->traced].step = step;
    s->traced++;
    s->coroThread = CthSelf();
    co_await std::suspend_always{};
  }
}

/* ------------------------------ checking ------------------------------ */

static int failures = 0;

static void check(int ok, const char *what) {
  CmiPrintf("[PE %d]   %-46s %s\n", CmiMyPe(), what, ok ? "ok" : "FAILED");
  if (!ok)
    failures++;
}

static void verify(void) {
  struct interleaveState *s = &CpvAccess(st);
  const int total = s->flows * s->steps;

  if (total <= MAX_PRINTED_TRACE) {
    CmiPrintf("[PE %d] trace:", CmiMyPe());
    for (int i = 0; i < s->traced; i++)
      CmiPrintf(" %d.%d", s->trace[i].id, s->trace[i].step);
    CmiPrintf("\n");
  }

  check(s->traced == total, "every coroutine ran every step");

  /* frame state: each coroutine's steps in order, exactly once each */
  int *next = (int *)calloc(s->flows, sizeof(int));
  int ordered = 1;
  for (int i = 0; i < s->traced; i++) {
    struct traceEntry *e = &s->trace[i];
    if (e->id < 0 || e->id >= s->flows || e->step != next[e->id]++)
      ordered = 0;
  }
  for (int i = 0; i < s->flows; i++)
    if (next[i] != s->steps)
      ordered = 0;
  check(ordered, "coroutine frames survived each suspension");

  /* interleaving: the scheduler ran between two steps of the same coroutine */
  int interleaved = 1;
  if (s->flows > 1)
    for (int i = 1; i < s->traced; i++)
      if (s->trace[i].id == s->trace[i - 1].id)
        interleaved = 0;
  check(interleaved, "scheduler dispatched others between steps");

  /* fairness: replay progress and track how far apart the flows drift */
  memset(next, 0, s->flows * sizeof(int));
  int maxSpread = 0;
  for (int i = 0; i < s->traced; i++) {
    next[s->trace[i].id]++;
    int lo = next[0], hi = next[0];
    for (int f = 1; f < s->flows; f++) {
      if (next[f] < lo)
        lo = next[f];
      if (next[f] > hi)
        hi = next[f];
    }
    if (hi - lo > maxSpread)
      maxSpread = hi - lo;
  }
  free(next);
  CmiPrintf("[PE %d]   max progress spread across flows: %d\n", CmiMyPe(),
            maxSpread);
  check(maxSpread <= 1, "ready queue rotated strictly round robin");

  check(s->coroThread == s->handlerThread,
        "coroutine ran on the handler's CthThread");

  CmiPrintf("[PE %d] %s (%d flows x %d steps)\n", CmiMyPe(),
            failures ? "FAILED" : "all checks passed", s->flows, s->steps);
}

/* ------------------------------- driving ------------------------------- */

static void coroHandler(void *msg) {
  struct interleaveState *s = &CpvAccess(st);
  s->handlerThread = CthSelf();

  std::coroutine_handle<> h =
      std::coroutine_handle<>::from_address(s->coros[((struct benchMsg *)msg)->index]);
  h.resume(); /* returns here as soon as the body hits its next co_await */

  if (!h.done()) {
    CmiPushPE(CmiMyRank(), msg);
    return;
  }

  if (++s->retired == s->flows) {
    verify();
    for (int i = 0; i < s->flows; i++) {
      std::coroutine_handle<>::from_address(s->coros[i]).destroy();
      CmiFree(s->msgs[i]);
    }
    free(s->trace);
    free(s->msgs);
    free(s->coros);
    if (failures)
      CmiAbort("coro_interleave: checks failed");
    CsdExitScheduler();
  }
}

static void test_init(int argc, char **argv) {
  (void)argc;

  CpvInitialize(struct interleaveState, st);
  CpvInitialize(int, coroHandlerIdx);
  CpvAccess(coroHandlerIdx) = CmiRegisterHandler((CmiHandler)coroHandler);

  struct interleaveState *s = &CpvAccess(st);
  s->flows = DEFAULT_FLOWS;
  s->steps = DEFAULT_STEPS;
  CmiGetArgInt(argv, "-flows", &s->flows);
  CmiGetArgInt(argv, "-steps", &s->steps);

  if (s->flows < 1 || s->steps < 1) {
    if (CmiMyPe() == 0)
      CmiPrintf("Error: need -flows >= 1 and -steps >= 1 (got %d, %d), "
                "exiting\n",
                s->flows, s->steps);
    CmiExit(1); /* only queues the exit, so stop here ourselves */
    return;
  }

  s->traced = 0;
  s->retired = 0;
  s->handlerThread = NULL;
  s->coroThread = NULL;
  s->trace =
      (struct traceEntry *)malloc(s->flows * s->steps * sizeof(struct traceEntry));
  s->msgs = (void **)malloc(s->flows * sizeof(void *));
  s->coros = (void **)malloc(s->flows * sizeof(void *));

  for (int i = 0; i < s->flows; i++) {
    struct benchMsg *m = (struct benchMsg *)CmiAlloc(sizeof(struct benchMsg));
    m->index = i;
    ((CmiMessageHeader *)m)->messageSize = sizeof(struct benchMsg);
    CmiSetHandler(m, CpvAccess(coroHandlerIdx));
    s->msgs[i] = m;
    s->coros[i] = stepCoro(i).h.address();
  }

  /* skip the communication thread */
  if (CmiMyRank() != CmiMyNodeSize())
    for (int i = 0; i < s->flows; i++)
      CmiPushPE(CmiMyRank(), s->msgs[i]);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, test_init);
  return 0;
}
