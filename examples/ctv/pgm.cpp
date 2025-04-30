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
  int myId = CpvAccess(data).nThreadStart++;

  // simple ctv test
  CtvDeclare(int, cthTest);
  CtvInitialize(int, cthTest);

  if (!CtvInitialized(cthTest))
    CmiAbort("CtvInitialize failed");

  CtvAccess(cthTest) = myId;

  if (CtvAccess(cthTest) != myId)
    CmiAbort("CtvAccess failed");

  if (myId == NSPAWN - 1) { // We're the last thread: leave
    double timeElapsed = CmiWallTimer() - CpvAccess(data).timeStart;
    CmiExit(0);
  }
}

void test_init(int argc, char **argv) {
  (void)argc;
  (void)argv;

  CpvInitialize(struct cthTestData, data);

  /* skip communication thread */
  CpvAccess(data).timeStart = CmiWallTimer();
  for (int i = 0; i < NSPAWN; i++) {
    CthThread yielder = CthCreate((CthVoidFn)runThread, 0, 160000);
    CthAwaken(yielder);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, test_init);
  return 0;
}
