/**
  Tests out Cth routines, by running a set of threads
  that create new threads.  The pattern of creations
  and yeilds is deterministic but complicated.

  Orion Lawlor, olawlor@acm.org, 2004/2/20
  */
#include "converse.h"
#include <stdio.h>
#include <stdlib.h>

#define NITER 10000 /* Each thread yields this many times */
#define NSPAWN 26   /* Spawn this many threads total */

/* Enable this define to get lots of debugging printouts */
#define VERBOSE(x) /* x */

class threadStackChecker {
  int val;   // Hash of current id and count
  int count; // Current count

  // Hash this id and count:
  static int fn(char id, int cnt) { return ((int)id) + (cnt << 16); }

public:
  threadStackChecker(char id) : val(fn(id, -1)), count(-1) {}

  void advance(char id, int cnt) {
    if (val != fn(id, count))
      CmiAbort("stack value corrupted!");
    if (++count != cnt)
      CmiAbort("stack count corrupted!");
    val = fn(id, count);
  }
};

struct cthTestData {
  double timeStart;
  int nThreadStart, nThreadFinish;
};

CpvStaticDeclare(struct cthTestData, data);

void runThread(void *msg) {
  (void)msg;
  char myId = 'A' + CpvAccess(data).nThreadStart++;
  threadStackChecker sc(myId);
  printf("Created thread %c at %p, mype = %d\n", myId, &myId, CmiMyPe());

  for (int iter = 0; iter < NITER; iter++) {
    VERBOSE(char buf[NITER + 2];
            for (int i = 0; i < NITER + 1; i++) buf[i] = ' '; buf[0] = myId;
            buf[1 + iter] = '0' + (iter % 10); buf[NITER + 1] = 0;
            printf("%s\n", buf);)
    if ((iter % 2) == 0) { // (rand()%NITER)<5) {
      CthYield();
    }
    if (iter == (NITER / 4) && myId < ('A' + NSPAWN - 1)) {
      CthThread nTh = CthCreate((CthVoidFn)runThread, 0, 160000);
      CthAwaken(nTh);
      CthYield();
    }
    sc.advance(myId, iter);
  }

  const int myFinish = ++(CpvAccess(data).nThreadFinish);
  if (myFinish == NSPAWN) { // We're the last thread: leave
    double timeElapsed = CmiWallTimer() - CpvAccess(data).timeStart;
    printf(" %d threads ran successfully (%.3f us per context switch)\n",
           myFinish, 1.0e6 * timeElapsed / (NITER * NSPAWN));
    CsdExitScheduler();
  }
}

void test_init(int argc, char **argv) {
  (void)argc;
  (void)argv;

  CpvInitialize(struct cthTestData, data);

  /* skip communication thread */
  if (CmiMyRank() != CmiMyNodeSize()) {
    CpvAccess(data).timeStart = CmiWallTimer();
    CthThread yielder = CthCreate((CthVoidFn)runThread, 0, 160000);
    CthAwaken(yielder);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, test_init);
  return 0;
}
