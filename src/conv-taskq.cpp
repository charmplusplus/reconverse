#include "conv-taskq.h"
#if CMK_SMP && CMK_TASKQUEUE
extern "C" void StealTask() {
  int random_rank = CrnRand() % (CmiMyNodeSize()-1);
  if (random_rank >= CmiMyRank())
    ++random_rank;
  void* msg = TaskQueueSteal((TaskQueue)CpvAccessOther(CsdTaskQueue, random_rank));
  if (msg != NULL) {
    TaskQueuePush((TaskQueue)CpvAccess(CsdTaskQueue), msg);
  }
}

static void TaskStealBeginIdle(void *dummy) {
  if (CmiMyNodeSize() > 1 && CpvAccess(isHelperOn))
    StealTask();
}

extern "C" void CmiTaskQueueInit() {
  if(CmiMyNodeSize() > 1) {
    CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,
        (CcdCondFn) TaskStealBeginIdle, NULL);

    CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
        (CcdCondFn) TaskStealBeginIdle, NULL);
  }
}
#endif