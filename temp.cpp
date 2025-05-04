#include "converse.h"
#include <atomic>

// number of tasks to fire
static const int N = 10000;

// thread‐local counter of completed tasks
thread_local static std::atomic<int> doneCount;

// the handler called when a task runs
void TaskHandler(void *msg) {
  // free our little “message”
  free(msg);

  // bump and check if we’re done
  if (doneCount.fetch_add(1, std::memory_order_relaxed) + 1 == N) {
    // shut down scheduler
    CsdExitScheduler();
  }
}

// module‐init (called once, before scheduler starts)
void ModuleInit(char **argv) {
  // register our handler
  int h = CmiRegisterHandler(TaskHandler);

  // initialise counter
  doneCount = 0;

  // enqueue N tasks
  for (int i = 0; i < N; ++i) {
    // allocate a “Converse message” header only
    void *m = malloc(CmiMsgHeaderSizeBytes);
    // tag it with our handler
    CmiSetHandler(m, h);
    // enqueue as a task
    CsdTaskEnqueue(m);
  }
}

int main(int argc, char **argv) {
  // register module init
  CmiRegisterModuleInit(ModuleInit);

  // now start up the PEs/threads and scheduler
  CmiStartThreads();
  return 0;
}