#include "conv-taskQ.h"

void StealTask() {
// start up timer if trace is enabled 
#if CMK_TRACE_ENABLED 
    double _start = CmiWallTimer();
#endif 
    
    //steal from a random PE on the same node
    int random_rank = CrnRand() % (CmiMyNodeSize()-1);
    if (random_rank == CmiMyRank()) {
        random_rank++; 
        if (random_rank >= CmiMyNodeSize()) {
            random_rank = 0; // wrap around if our random_selected node is the same as our own 
                             // and we are the last PE on our node
        }
    }

#if CMK_TRACE_ENABLED
    char s[10];
    snprintf( s, sizeof(s), "%d", random_rank );
    traceUserSuppliedBracketedNote(s, TASKQ_QUEUE_STEAL_EVENTID, _start, CmiWallTimer());
#endif
    void* msg = TaskQueueSteal((TaskQueue*)CpvAccessOther(task_q, random_rank));
    if (msg != NULL) {
        TaskQueuePush((TaskQueue*)CpvAccess(task_q), msg);
    }
#if CMK_TRACE_ENABLED
    traceUserSuppliedBracketedNote(s, TASKQ_STEAL_EVENTID, _start, CmiWallTimer());
#endif
}

// this function is passed into CcdCallOnConditionKeep 
static void TaskStealBeginIdle() {
    // can discuss whether we need to add the isHelper csv variable that is in old converse. 
    // not going to add it for now, because it's turned/left on by default in old converse 
    if (CmiMyNodeSize() > 1) {
        StealTask();
    }
}

// each pe will call this function because each pe has its own task queue 
void CmiTaskQueueInit() {
    // makes sure that the node has more than one PE, because we can only steal
    // from other PE's that share the same node

    //initlialize the task queue for this PE 
    CpvInitialize(TaskQueue*, task_q);
    CpvAccess(task_q) = TaskQueueCreate();

    if(CmiMyNodeSize() > 1) { 
        CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, (CcdCondFn) TaskStealBeginIdle, NULL);
    
        CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE, (CcdCondFn) TaskStealBeginIdle, NULL);
    }

#if CMK_TRACE_ENABLED
    traceRegisterUserEvent("taskq create work", TASKQ_CREATE_EVENTID);
    traceRegisterUserEvent("taskq work", TASKQ_WORK_EVENTID);
    traceRegisterUserEvent("taskq steal", TASKQ_STEAL_EVENTID);
    traceRegisterUserEvent("taskq from queue steal", TASKQ_QUEUE_STEAL_EVENTID);
#endif

}