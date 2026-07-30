/**
  Measures how fast the Converse scheduler dispatches ready user-level threads
  (ULTs).

  A "dispatch" is one trip through the scheduler's ready path: CthAwaken pushes
  the thread's token onto this PE's queue, CsdScheduler pops it, and
  CthResumeNormalThread resumes the thread. Each PE runs -threads ULTs that each
  call CthYield() -yields times, so a repetition performs

      threads * (yields + 1)

  dispatches: one for the initial awaken, plus one per yield.

  -threads sets how many ULTs are runnable at once, i.e. the depth of the ready
  queue. Sweeping it shows how the dispatch rate holds up as the queue fills and
  the working set of thread stacks stops fitting in cache.

  CthCreate is deliberately kept outside the timed region and reported
  separately: it mallocs a stack, which would otherwise dominate at low -yields.

  Usage: reconverse_sched_rate +pe N [-threads T] [-yields Y] [-reps R] [-stack B]
  */
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_THREADS 64
#define DEFAULT_YIELDS 1000
#define DEFAULT_REPS 5
#define DEFAULT_STACK 65536

struct schedBench {
  int nthreads;      /* ULTs runnable at once */
  int nyields;       /* CthYield() calls per ULT */
  int nreps;         /* timed repetitions, after one warmup rep */
  int stack;         /* per-ULT stack size in bytes */
  int rep;           /* current rep, -1 during warmup */
  int running;       /* ULTs still alive in this rep */
  double repStart;   /* wall time when the timed region opened */
  double createTime; /* CthCreate time for this rep, excluded from the timing */
  double best;       /* lowest us/dispatch over the timed reps */
  double total;      /* summed us/dispatch over the timed reps */
};

CpvStaticDeclare(struct schedBench, bench);

static void startRep(void);
static void finishRep(void);

static void yielderFn(void *arg) {
  (void)arg;
  const int nyields = CpvAccess(bench).nyields;
  for (int i = 0; i < nyields; i++)
    CthYield();

  /* ULTs on a PE are cooperative, so this needs no atomics */
  if (--CpvAccess(bench).running == 0)
    finishRep();
}

static void startRep(void) {
  struct schedBench *d = &CpvAccess(bench);
  CthThread *threads = (CthThread *)malloc(d->nthreads * sizeof(CthThread));
  d->running = d->nthreads;

  double t0 = CmiWallTimer();
  for (int i = 0; i < d->nthreads; i++)
    threads[i] = CthCreate((CthVoidFn)yielderFn, 0, d->stack);
  d->createTime = CmiWallTimer() - t0;

  /* time only the awaken/dispatch path */
  d->repStart = CmiWallTimer();
  for (int i = 0; i < d->nthreads; i++)
    CthAwaken(threads[i]);
  free(threads);
  /* the ULTs free themselves as they exit, so there is nothing to reclaim */
}

static void finishRep(void) {
  struct schedBench *d = &CpvAccess(bench);
  double elapsed = CmiWallTimer() - d->repStart;
  double dispatches = (double)d->nthreads * (d->nyields + 1);
  double usEach = 1.0e6 * elapsed / dispatches;

  if (d->rep >= 0) { /* the warmup rep is measured but not recorded */
    d->total += usEach;
    if (usEach < d->best)
      d->best = usEach;
    CmiPrintf("[PE %d] rep %d: %.0f dispatches in %.6f s -> %.3f M/s, "
              "%.4f us each (create %.3f us/thread)\n",
              CmiMyPe(), d->rep, dispatches, elapsed,
              1.0e-6 * dispatches / elapsed, usEach,
              1.0e6 * d->createTime / d->nthreads);
  }

  d->rep++;
  if (d->rep < d->nreps) {
    startRep();
    return;
  }

  double mean = d->total / d->nreps;
  CmiPrintf("[PE %d] threads=%d yields=%d: best %.4f us/dispatch "
            "(%.3f M dispatches/s), mean %.4f us\n",
            CmiMyPe(), d->nthreads, d->nyields, d->best, 1.0 / d->best, mean);
  CmiPrintf("Format: DATA,sched_rate,{pe},{threads},{yields},{best "
            "us/dispatch},{mean us/dispatch},{best M dispatch/s}\n");
  CmiPrintf("DATA,sched_rate,%d,%d,%d,%f,%f,%f\n", CmiMyPe(), d->nthreads,
            d->nyields, d->best, mean, 1.0 / d->best);

  CsdExitScheduler();
}

static void bench_init(int argc, char **argv) {
  (void)argc;

  CpvInitialize(struct schedBench, bench);
  struct schedBench *d = &CpvAccess(bench);
  d->nthreads = DEFAULT_THREADS;
  d->nyields = DEFAULT_YIELDS;
  d->nreps = DEFAULT_REPS;
  d->stack = DEFAULT_STACK;
  CmiGetArgInt(argv, "-threads", &d->nthreads);
  CmiGetArgInt(argv, "-yields", &d->nyields);
  CmiGetArgInt(argv, "-reps", &d->nreps);
  CmiGetArgInt(argv, "-stack", &d->stack);

  if (d->nthreads < 1 || d->nyields < 0 || d->nreps < 1 || d->stack < 4096) {
    if (CmiMyPe() == 0)
      CmiPrintf("Error: need -threads >= 1, -yields >= 0, -reps >= 1 and "
                "-stack >= 4096 (got %d, %d, %d, %d), exiting\n",
                d->nthreads, d->nyields, d->nreps, d->stack);
    CmiExit(1); /* only queues the exit, so stop here ourselves */
    return;
  }

  d->rep = -1;
  d->running = 0;
  d->best = 1.0e30;
  d->total = 0.0;

  if (CmiMyPe() == 0)
    CmiPrintf("ULT scheduling rate: %d threads x %d yields, %d reps "
              "(+1 warmup), %d byte stacks, on %d PEs\n",
              d->nthreads, d->nyields, d->nreps, d->stack, CmiNumPes());

  /* skip the communication thread */
  if (CmiMyRank() != CmiMyNodeSize())
    startRep();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, bench_init);
  return 0;
}
