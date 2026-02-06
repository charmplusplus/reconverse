/* Test of Converse task queue work stealing mechanism
 * launch tasks to calculate "division" with steal queue
 * and simultaneously send a message in a circle using normal send
 * "division=" start at some number, divide into 2 tasks of 1/2 the number
 * until reaching 1
*/

#include "converse.h"

#define MAX_TASK_NUM 512
#define MAX_RING_HOPS 1000
CpvDeclare(int, divideHandlerId);
CpvDeclare(int, ringHandlerId);

struct Message {
  CmiMessageHeader header;
  int num; //ring hops
};

struct ConverseTaskMsg {
  char converseHdr[CmiMsgHeaderSizeBytes]; // for system use
  int num; //task number
};

void divide_handler(void *msg) {
  //CmiPrintf("[PE %d] Divide handler called\n", CmiMyPe());
  ConverseTaskMsg *taskMsg = (ConverseTaskMsg *)msg;
  int num = taskMsg->num;

  if (num > 1) {
    // create two new tasks
    for (int i = 0; i < 2; i++) {
      ConverseTaskMsg *newTaskMsg = (ConverseTaskMsg *)CmiAlloc(sizeof(ConverseTaskMsg));
      CmiMessageHeader *header = (CmiMessageHeader *)newTaskMsg;
      header->handlerId = CpvAccess(divideHandlerId);
      header->messageSize = sizeof(ConverseTaskMsg);
      newTaskMsg->num = num / 2;
      CsdTaskEnqueue((void *)newTaskMsg);
    }
  } /*else: no task created*/

  CmiFree(msg);
}

// rings within the process only
void ring_handler(void *msg) {
  //CmiPrintf("[PE %d] Ring handler called\n", CmiMyPe());
  Message *ring_msg = (Message *)msg;
  ring_msg->num--;
  if (ring_msg->num == 0) {
    CmiPrintf("[PE %d] Ring completed\n", CmiMyPe());
    CmiFree(msg);
    CmiExit(0);
  }
  else {
    CmiMessageHeader *header = (CmiMessageHeader *)msg;
    int next_pe = (CmiMyRank() + 1) % CmiMyNodeSize();
    CmiSyncSendAndFree(next_pe, header->messageSize, msg);
  }
}

CmiStartFn mymain(int argc, char **argv){
    CpvInitialize(int, divideHandlerId);
    CpvAccess(divideHandlerId) = CmiRegisterHandler(divide_handler);
    CpvInitialize(int, ringHandlerId);
    CpvAccess(ringHandlerId) = CmiRegisterHandler(ring_handler);

    //launch ring task on rank 0
    if(CmiMyRank() == 0)
    {
      CmiPrintf("[PE %d] Launching ring message\n", CmiMyPe());
      Message *msg = (Message *)CmiAlloc(sizeof(Message));
      msg->num = MAX_RING_HOPS * CmiMyNodeSize();
      CmiMessageHeader *header = (CmiMessageHeader *)msg;
      header->handlerId = CpvAccess(ringHandlerId);
      header->messageSize = sizeof(Message);
      CmiSyncSendAndFree((CmiMyRank() + 1) % CmiMyNodeSize(),
                          header->messageSize, msg);
    }
    //launch divide tasks
    for(int i=1; i<MAX_TASK_NUM; i*=2)
    {
      //CmiPrintf("[PE %d] Launching divide task with num=%d\n", CmiMyPe(), i);
      ConverseTaskMsg *taskMsg = (ConverseTaskMsg *)CmiAlloc(sizeof(ConverseTaskMsg));
      CmiMessageHeader *header = (CmiMessageHeader *)taskMsg;
      header->handlerId = CpvAccess(divideHandlerId);
      header->messageSize = sizeof(ConverseTaskMsg);
      taskMsg->num = i;
      CsdTaskEnqueue((void *)taskMsg);
    }

    return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
