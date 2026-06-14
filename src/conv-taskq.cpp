#include "converse.h"

/* Check if the local task queue has tasks available */
extern "C" int TaskQueueHasWork(void) {
  TaskQueue q = (TaskQueue)CpvAccess(CsdTaskQueue);
  return q->tail > q->head;
}

/* Pop a task from the local task queue if available */
extern "C" void* TaskQueuePopLocal(void) {
  return TaskQueuePop((TaskQueue)CpvAccess(CsdTaskQueue));
}

/* Push a task to the local task queue */
extern "C" void TaskQueuePushLocal(void *task) {
  TaskQueuePush((TaskQueue)CpvAccess(CsdTaskQueue), task);
}

/* Attempt to steal a task from a random PE on the same node */
extern "C" void StealTask(void) {
  int node_size = CmiMyNodeSize();
  
  if (node_size <= 1)
    return;
  
  /* Pick a random rank on the same node (excluding ourselves) */
  int random_rank = CrnRand() % (node_size - 1);
  if (random_rank >= CmiMyRank())
    ++random_rank;
  
  /* Attempt to steal from the random PE */
  void* stolen_task = TaskQueueSteal((TaskQueue)CpvAccessOther(CsdTaskQueue, random_rank));
  
  /* If we successfully stole a task, add it to our local queue */
  if (stolen_task != NULL) {
    TaskQueuePush((TaskQueue)CpvAccess(CsdTaskQueue), stolen_task);
  }
}

/* Callback when processor becomes idle - attempt work stealing */
static void TaskStealBeginIdle(void *dummy) {
  if (CmiMyNodeSize() > 1) {
    StealTask();
  }
}

/* Initialize the task queue stealing mechanism */
extern "C" void CmiTaskQueueInit(void) {
  if (CmiMyNodeSize() > 1) {
    /* Register callbacks for when processor becomes idle */
    CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,
        (CcdCondFn) TaskStealBeginIdle, NULL);

    CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
        (CcdCondFn) TaskStealBeginIdle, NULL);
  }
}

/* Main task processing loop - check local queue and steal if idle */
extern "C" int ProcessLocalTasks(void) {
  int tasks_processed = 0;
  
  /* Process all tasks currently in the local queue */
  while (TaskQueueHasWork()) {
    void* task = TaskQueuePopLocal();
    if (task == NULL)
      break;
    
    /* Execute the task (task should be a message handler or similar) */
    CmiHandleMessage(task);
    tasks_processed++;
  }
  
  /* If no local tasks, attempt to steal from other PEs */
  if (tasks_processed == 0 && CmiMyNodeSize() > 1) {
    StealTask();
  }
  
  return tasks_processed;
}
