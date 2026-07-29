/**
  Measures ULT context switch time as a function of the number of concurrent
  ULTs ("flows"): one row per flow count, sweeping -min_flows to -max_flows by
  doubling. Plot column 4 (flows) against column 5 (us/switch).

  Two costs are reported at every point, because "context switch" has two
  meanings in this runtime and they differ by more than 10x:

    direct    The `flows` ULTs are wired into a ring and hand control to their
              successor with CthResume(), a single swapcontext with no queue
              involved. This is the switch primitive alone, and it is the curve
              that shows the stack working set growing: the only thing that
              changes across the sweep is how many stacks the switches touch.

    scheduler The same `flows` ULTs hand control to each other with CthYield(),
              which is what application code writes: awaken self, suspend to the
              scheduling thread, let the scheduler pop the next token and resume
              it. That is two swapcontexts plus a queue push/pop and a handler
              dispatch.

  Both phases hold the total switch count per rep fixed at -switches regardless
  of the flow count, so every point on the curve does the same amount of work
  and only the concurrency varies.

  Note that memory use is flows * -stack bytes, so raising -max_flows past a few
  thousand at the default stack size wants a look at available RAM.

  Usage: reconverse_ctxswitch +pe N [-min_flows F] [-max_flows F]
                                    [-switches S] [-reps R] [-stack B]
  */
#include "converse.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_MIN_FLOWS 2
#define DEFAULT_MAX_FLOWS 1024
#define DEFAULT_SWITCHES 200000
#define DEFAULT_REPS 5
#define DEFAULT_STACK 65536
#define WARMUP_LAPS 100

struct ctxBench {
  int minFlows, maxFlows;
  int nswitches; /* switches per timed rep, held fixed across the sweep */
  int nreps;
  int stack;
  int flows;       /* flow count at the current point of the sweep */
  int alive;       /* scheduler-phase peers still running */
  int done;        /* tells the scheduler-phase peers to stop yielding */
  CthThread *ring; /* `flows` entries; ring[0] is the driver itself */
};

CpvStaticDeclare(struct ctxBench, bench);

/* ------------------- direct: CthResume around a ring ------------------- */

static void ringFn(void *arg) {
  struct ctxBench *d = &CpvAccess(bench);
  long i = (long)(intptr_t)arg;
  /* safe to read the ring here: nothing resumes us until it is fully built */
  CthThread next = d->ring[(i + 1) % d->flows];
  /* Runs until the driver stops going around the ring, then stays suspended
     inside CthResume; the driver frees us afterwards. */
  for (;;)
    CthResume(next);
}

static double directPoint(void) {
  struct ctxBench *d = &CpvAccess(bench);
  const int flows = d->flows;
  int laps = d->nswitches / flows;
  if (laps < 1)
    laps = 1;

  d->ring = (CthThread *)malloc(flows * sizeof(CthThread));
  d->ring[0] = CthSelf();
  for (long i = 1; i < flows; i++)
    d->ring[i] = CthCreate((CthVoidFn)ringFn, (void *)(intptr_t)i, d->stack);

  /* also runs each ring member up to its loop for the first time */
  for (int i = 0; i < WARMUP_LAPS; i++)
    CthResume(d->ring[1]);

  double best = 1.0e30;
  for (int rep = 0; rep < d->nreps; rep++) {
    double t0 = CmiWallTimer();
    for (int lap = 0; lap < laps; lap++)
      CthResume(d->ring[1]);
    double elapsed = CmiWallTimer() - t0;

    /* one lap is `flows` switches: 0->1, 1->2, ... (flows-1)->0 */
    double usEach = 1.0e6 * elapsed / ((double)laps * flows);
    if (usEach < best)
      best = usEach;
  }

  for (int i = 1; i < flows; i++)
    CthFree(d->ring[i]); /* suspended in CthResume, never resumed again */
  free(d->ring);
  d->ring = NULL;
  return best;
}

/* ---------------- scheduler-mediated: CthYield round robin ---------------- */

static void yieldPeerFn(void *arg) {
  (void)arg;
  struct ctxBench *d = &CpvAccess(bench);
  while (!d->done)
    CthYield();
  d->alive--;
}

static double yieldPoint(void) {
  struct ctxBench *d = &CpvAccess(bench);
  const int flows = d->flows;
  int per = d->nswitches / flows; /* yields by the driver, per rep */
  if (per < 1)
    per = 1;

  d->done = 0;
  d->alive = flows - 1;
  for (int i = 1; i < flows; i++)
    CthAwaken(CthCreate((CthVoidFn)yieldPeerFn, 0, d->stack));

  for (int i = 0; i < WARMUP_LAPS; i++)
    CthYield();

  double best = 1.0e30;
  for (int rep = 0; rep < d->nreps; rep++) {
    double t0 = CmiWallTimer();
    for (int i = 0; i < per; i++)
      CthYield();
    double elapsed = CmiWallTimer() - t0;

    /* The ready queue is FIFO and holds exactly these `flows` ULTs, so they
       rotate strictly: every one of them yields once per driver yield, giving
       per * flows handoffs inside the window. */
    double usEach = 1.0e6 * elapsed / ((double)per * flows);
    if (usEach < best)
      best = usEach;
  }

  d->done = 1;
  while (d->alive > 0) /* let every peer wake up once and exit */
    CthYield();
  return best;
}

/* ------------------------------- sweep ------------------------------- */

static void driverFn(void *arg) {
  (void)arg;
  struct ctxBench *d = &CpvAccess(bench);

  for (int f = d->minFlows;; f *= 2) {
    d->flows = f;
    double direct = directPoint();
    double sched = yieldPoint();

    CmiPrintf("[PE %d] flows=%5d: direct %.4f us/switch (%.3f M/s), "
              "scheduler %.4f us/switch (%.3f M/s)\n",
              CmiMyPe(), f, direct, 1.0 / direct, sched, 1.0 / sched);
    CmiPrintf("DATA,ctxswitch,%d,%d,%f,%f\n", CmiMyPe(), f, direct, sched);

    if (f > d->maxFlows / 2) /* doubling again would pass max_flows */
      break;
  }

  CsdExitScheduler();
}

static void bench_init(int argc, char **argv) {
  (void)argc;

  CpvInitialize(struct ctxBench, bench);
  struct ctxBench *d = &CpvAccess(bench);
  d->minFlows = DEFAULT_MIN_FLOWS;
  d->maxFlows = DEFAULT_MAX_FLOWS;
  d->nswitches = DEFAULT_SWITCHES;
  d->nreps = DEFAULT_REPS;
  d->stack = DEFAULT_STACK;
  CmiGetArgInt(argv, "-min_flows", &d->minFlows);
  CmiGetArgInt(argv, "-max_flows", &d->maxFlows);
  CmiGetArgInt(argv, "-switches", &d->nswitches);
  CmiGetArgInt(argv, "-reps", &d->nreps);
  CmiGetArgInt(argv, "-stack", &d->stack);

  /* one flow cannot switch to anything but itself, so the ring needs two */
  if (d->minFlows < 2 || d->maxFlows < d->minFlows || d->nswitches < 1 ||
      d->nreps < 1 || d->stack < 4096) {
    if (CmiMyPe() == 0)
      CmiPrintf("Error: need -min_flows >= 2, -max_flows >= min_flows, "
                "-switches >= 1, -reps >= 1 and -stack >= 4096 "
                "(got %d, %d, %d, %d, %d), exiting\n",
                d->minFlows, d->maxFlows, d->nswitches, d->nreps, d->stack);
    CmiExit(1); /* only queues the exit, so stop here ourselves */
    return;
  }

  d->flows = 0;
  d->alive = 0;
  d->done = 0;
  d->ring = NULL;

  if (CmiMyPe() == 0) {
    CmiPrintf("ULT context switch vs concurrency: flows %d..%d (doubling), "
              "%d switches x %d reps per point, %d byte stacks, on %d PEs\n",
              d->minFlows, d->maxFlows, d->nswitches, d->nreps, d->stack,
              CmiNumPes());
    CmiPrintf("Format: DATA,ctxswitch,{pe},{flows},{direct us/switch},"
              "{scheduler us/switch}\n");
  }

  /* skip the communication thread */
  if (CmiMyRank() != CmiMyNodeSize())
    CthAwaken(CthCreate((CthVoidFn)driverFn, 0, d->stack));
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, bench_init);
  return 0;
}
