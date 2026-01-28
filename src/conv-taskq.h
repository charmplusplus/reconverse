#ifndef _CK_TASKQ_H_
#define _CK_TASKQ_H_
#include "converse.h"
#include "taskqueue.h"

CpvExtern(TaskQueue, CsdTaskQueue);

#ifdef __cplusplus
extern "C" {
#endif

/* Steal a task from another PE on the same node */
void StealTask(void);

/* Initialize task queue work-stealing callbacks */
void CmiTaskQueueInit(void);

/* Check if the local task queue has work available */
int TaskQueueHasWork(void);

/* Pop a task from the local task queue */
void* TaskQueuePopLocal(void);

/* Push a task to the local task queue */
void TaskQueuePushLocal(void *task);

/* Process all tasks in the local queue and steal if idle */
int ProcessLocalTasks(void);

#ifdef __cplusplus
}
#endif
#endif