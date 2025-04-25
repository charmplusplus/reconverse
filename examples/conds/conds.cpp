#include "converse.h"
#include <stdio.h>
#include <pthread.h>

CpvDeclare(int, test);
CpvDeclare(int, exitHandlerId);
CpvDeclare(int, nodeHandlerId);
CpvDeclare(int, 1sHandlerId);
CpvDeclare(int, 5sHandlerId);
CpvDeclare(int, 10sHandlerId);

struct Message
{
  CmiMessageHeader header;
};

void stop_handler(void *vmsg)
{
  CsdExitScheduler();
}

void shortHandler(void *vmsg)
{
  printf("1s HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(), CmiMyRank());
}

void mediumHandler(void *vmsg)
{
  printf("5s HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(), CmiMyRank());
}

void callAfter7s(void *vmsg)
{
  printf("7s (ccd call after) HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(), CmiMyRank());
}

void longHandler(void *vmsg)
{
  printf("10s HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(), CmiMyRank());
  Message *msg = new Message;
  msg->header.handlerId = CpvAccess(exitHandlerId);
  msg->header.messageSize = sizeof(Message);
  msg->header.destPE = CmiMyRank();
  CmiSyncSendAndFree(CmiMyRank(), msg->header.messageSize, msg);
}

CmiStartFn mymain(int argc, char **argv)
{

  CpvInitialize(int, exitHandlerId);
  CpvAccess(exitHandlerId) = CmiRegisterHandler(stop_handler);
  CpvInitialize(int, 1sHandlerId);
  CpvAccess(1sHandlerId) = CcdCallOnCondition(CcdPERIODIC_1s, shortHandler, 0);
  CpvInitialize(int, 5sHandlerId);
  CpvAccess(5sHandlerId) = CcdCallOnCondition(CcdPERIODIC_5s, mediumHandler, 0);
  CpvInitialize(int, 10sHandlerId);
  CpvAccess(10sHandlerId) = CcdCallOnCondition(CcdPERIODIC_10s, longHandler, 0);
  CcdCallFnAfter(callAfter7s, 0, 7000);

  return 0;
}

int main(int argc, char **argv)
{
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}