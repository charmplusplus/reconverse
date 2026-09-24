// Thread bookkeeping Charm++ uses around Cth threads, which the cth test
// does not touch: CthAddListener (suspend/resume/free callbacks),
// CthSetEventInfo, CthSetThreadID/CthGetThreadID, CthAwakenPrio.
// Every PE runs two worker threads in turn; each yields a fixed number of
// times. A listener must see one resume when its thread first runs, then
// one suspend and one resume per yield (as in classic Converse,
// CthBaseResume fires the resume listener on every switch into the thread).
// The free callback runs when the thread is actually freed; a finished
// thread is parked in a one-entry pool and freed when the next finished
// thread replaces it (classic does the same), so the first worker's free
// listener must have run by the time the second has finished.
#include "converse.h"
#include <cstdio>

static const int kYields = 5;

struct Counts {
  int suspend, resume, freed;
};

struct Worker {
  Counts counts;
  CthThreadListener listener;
  CthThread thread;
  int done;
};
struct PeState {
  Worker w[2];
};
CpvDeclare(PeState, st);

static void start_worker(int k);

static void on_suspend(struct CthThreadListener *l) {
  ((Counts *)l->data)->suspend++;
}
static void on_resume(struct CthThreadListener *l) {
  ((Counts *)l->data)->resume++;
}
static void on_free(struct CthThreadListener *l) {
  ((Counts *)l->data)->freed++;
}

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("cth_listener: %s (PE %d)", what, CmiMyPe());
}

static void worker_fn(void *arg) {
  int k = (int)(size_t)arg;
  Worker &w = CpvAccess(st).w[k];
  check(CthSelf() == w.thread, "CthSelf inside the worker");
  check(w.counts.resume == 1 && w.counts.suspend == 0,
        "resume listener did not fire once when the thread first ran");
  CmiObjId *id = CthGetThreadID(CthSelf());
  check(id->id[0] == 11 + k && id->id[1] == 22 && id->id[2] == 33,
        "thread id not the one set with CthSetThreadID");
  for (int i = 0; i < kYields; i++) {
    CthYield();
    check(w.counts.suspend == i + 1, "suspend callback count after yield");
    check(w.counts.resume == i + 2, "resume callback count after yield");
  }
  w.done = 1;
  if (k == 0)
    start_worker(1);
  // Returning ends the thread; its free listener runs when the runtime
  // frees it, which finish() checks for worker 0.
}

static void finish(void *arg, double) {
  PeState &s = CpvAccess(st);
  check(s.w[0].done && s.w[1].done, "a worker did not finish");
  check(s.w[0].counts.freed == 1,
        "free listener of the first worker did not run exactly once");
  check(s.w[1].counts.freed <= 1,
        "free listener of the second worker ran twice");
  CmiPrintf("[%d] listeners: worker 0 saw %d suspends, %d resumes, freed %d; "
            "worker 1 saw %d suspends, %d resumes, freed %d; ok\n",
            CmiMyPe(), s.w[0].counts.suspend, s.w[0].counts.resume,
            s.w[0].counts.freed, s.w[1].counts.suspend, s.w[1].counts.resume,
            s.w[1].counts.freed);
  CsdExitScheduler();
}

static void start_worker(int k) {
  Worker &w = CpvAccess(st).w[k];
  w.counts = {0, 0, 0};
  w.done = 0;
  w.thread = CthCreate(worker_fn, (void *)(size_t)k, 0);
  check(w.thread != NULL, "CthCreate failed");
  CthSetStrategyDefault(w.thread);
  CthSetThreadID(w.thread, 11 + k, 22, 33);
  CthSetEventInfo(w.thread, 7, CmiMyPe());
  w.listener.suspend = on_suspend;
  w.listener.resume = on_resume;
  w.listener.free = on_free;
  w.listener.data = &w.counts;
  CthAddListener(w.thread, &w.listener);
  unsigned int prio = 0;
  CthAwakenPrio(w.thread, CQS_QUEUEING_IFIFO, 32, &prio);
}

static void mymain(int argc, char **argv) {
  CpvInitialize(PeState, st);
  start_worker(0);
  // Both workers run from the scheduler; check the outcome shortly after.
  CcdCallFnAfter(finish, NULL, 500);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
