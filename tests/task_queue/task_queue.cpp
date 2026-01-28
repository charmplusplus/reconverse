/* Test of Converse task queue work stealing mechanism
 * launch tasks to calculate "division" with steal queue
 * and simultaneously send a message in a circle using normal send
 * "division=" start at some number, divide into 2 tasks of 1/2 the number
 * until reaching 1
*/

#include "converse.h"

#define MAX_TASK_NUM 512
CpvDeclare(int, exitHandlerId);
CpvDeclare(int, divideHandlerId);
CpvDeclare(int, ringHandlerId);

struct Message {
  CmiMessageHeader header;
};

struct ConverseTaskMsg {
  char converseHdr[CmiMsgHeaderSizeBytes]; // for system use
  int num; //task number
};

//end after 10 seconds regardless of progress
void exitHandler(void *vmsg) {
  printf("Exiting Converse after 10 seconds\n");
  CmiExit(0);
}

void divide_handler(void *msg) {
  ConverseTaskMsg *taskMsg = (ConverseTaskMsg *)msg;
  int num = taskMsg->num;

  if (num > 1) {
    // create two new tasks
    for (int i = 0; i < 2; i++) {
      ConverseTaskMsg *newTaskMsg = (ConverseTaskMsg *)CmiAlloc(sizeof(ConverseTaskMsg));
      newTaskMsg->num = num / 2;
      CsdTaskEnqueue((void *)newTaskMsg);
    }
  } /*else: no task created*/

  CmiFree(msg);
}

// rings within the process only
void ring_handler(void *msg) {
  CmiMessageHeader *header = (CmiMessageHeader *)msg;
  int next_pe = (CmiMyRank() + 1) % CmiMyNodeSize();
  CmiSyncSendAndFree(next_pe, header->messageSize, msg);
}

CmiStartFn mymain(int argc, char **argv){
    CpvInitialize(int, exitHandlerId);
    CpvAccess(exitHandlerId) = CmiRegisterHandler(exitHandler);
    CpvInitialize(int, divideHandlerId);
    CpvAccess(divideHandlerId) = CmiRegisterHandler(divide_handler);
    CpvInitialize(int, ringHandlerId);
    CpvAccess(ringHandlerId) = CmiRegisterHandler(ring_handler);

    //launch ring task
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    CmiMessageHeader *header = (CmiMessageHeader *)msg;
    header->handlerId = CpvAccess(ringHandlerId);
    header->messageSize = sizeof(Message);
    CmiSyncSendAndFree((CmiMyRank() + 1) % CmiMyNodeSize(),
                        header->messageSize, msg);
    //launch divide tasks
    for(int i=0; i<MAX_TASK_NUM; i*=2)
    {
      ConverseTaskMsg *taskMsg = (ConverseTaskMsg *)CmiAlloc(sizeof(ConverseTaskMsg));
      taskMsg->num = MAX_TASK_NUM;
      CsdTaskEnqueue((void *)taskMsg);
    }

}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}