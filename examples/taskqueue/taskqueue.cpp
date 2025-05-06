#include "converse.h"
#include <pthread.h>
#include <stdio.h>

#define K 1600
#define X 10000

CsvDeclare(int, globalCounter);
CpvDeclare(int, tasksExecuted);

int handlerID;
int print_handlerID;

struct Message {
  CmiMessageHeader header;
  int data[1];
};

void print_results_handler_func(void* vmsg) {
    printf("PE %d executed %d tasks.\n", CmiMyRank(), CpvAccess(tasksExecuted));
    CmiNodeBarrier();
    if (CmiMyRank() == 0) {
        CmiExit(0);
    }
}

void handler_func(void *vmsg) {
  Message* incoming_msg = (Message*)vmsg;  
  //printf("PE %d pinged this function with data index: %d.\n", CmiMyRank(), incoming_msg->data[0]);
  
  // do dummy work 
  for (int i = 0; i < X; i++) {
    CmiWallTimer();
  }

  CpvAccess(tasksExecuted)++; 
  CsvAccess(globalCounter) = CsvAccess(globalCounter) - 1;
  if (CsvAccess(globalCounter) == 0) {
    Message* msg = new Message; 
    msg->header.handlerId = print_handlerID; 
    msg->header.messageSize = sizeof(Message);
    printf("All tasks have been executed.\n");
    CmiSyncBroadcastAllAndFree(sizeof(Message), msg);
  }
}

CmiStartFn mymain(int argc, char **argv) {
    CpvInitialize(int, tasksExecuted);
    CpvAccess(tasksExecuted) = 0; 

    handlerID = CmiRegisterHandler(handler_func);
    print_handlerID = CmiRegisterHandler(print_results_handler_func);

    if (CmiMyPe() == 0) {
        CsvInitialize(int, globalCounter);
        CsvAccess(globalCounter) = K;
        for (int i = 0; i < K; i++) {
            Message* newmsg = new Message; 
            newmsg->data[0] = i; 
            newmsg->header.messageSize = sizeof(Message);
            newmsg->header.handlerId = handlerID;
            CmiTaskQueueSyncSend(CmiMyRank(), sizeof(Message), newmsg);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
