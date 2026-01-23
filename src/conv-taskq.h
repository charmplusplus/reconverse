#ifndef _CK_TASKQ_H_
#define _CK_TASKQ_H_
#include "converse.h"
#include "taskqueue.h"

#ifdef __cplusplus
extern "C" {
#endif
void StealTask();
void CmiTaskQueueInit();
#ifdef __cplusplus
}
#endif
#endif