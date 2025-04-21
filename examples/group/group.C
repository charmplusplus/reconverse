#include "converse.h"
#include <stdio.h>
#include <pthread.h>

CpvDeclare(int, exitHandlerId);
CpvDeclare(CmiGroup, group);

struct Message
{
  CmiMessageHeader header;
};

void stop_handler(void *vmsg)
{
  CsdExitScheduler();
}

void ping_handler(void *vmsg)
{
  printf("Group send received on pe %i\n", CmiMyPe());
  CmiExit(0);
}

CmiStartFn mymain(int argc, char **argv)
{
  printf("My PE is %d\n", CmiMyRank());

  int handlerId = CmiRegisterHandler(ping_handler);

  if(CmiNumPes() < 3)
  {
    printf("This example requires at least 3 PEs\n");
    CmiAbort("Not enough PEs");
  }

  CpvInitialize(CmiGroup, group);

  if (CmiMyPe() == 0)
  {
    //make a group
    const int group_size = 3;
    int pes[group_size] = {0, 1, 2};
    CpvAccess(group) = CmiEstablishGroup(group_size, pes);
  }

  CmiNodeBarrier(); // wait for all PEs to finish

  if (CmiMyPe() == 0)
  {
     // create a message
     Message *msg = new Message;
     msg->header.messageSize = sizeof(Message);
     msg->header.handlerId = handlerId;
     CmiSyncMulticastAndFree(CpvAccess(group), sizeof(Message), msg);
  }

     //CmiExit(0);
  return 0;
}

int main(int argc, char **argv)
{
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}