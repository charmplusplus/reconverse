#include "converse.h"
#include <stdio.h>
#include <pthread.h>
#include <iostream> 

CpvDeclare(int, exitHandlerId);
static int doneCounter = 0; 

struct Message
{
  CmiMessageHeader header;
  int addition_data; 
  int prod_data; 
};

void* mergeAddition(int *size, void* data, void** remote, int n)
{
  Message* localMessage = (Message* )data; 
  int localSum = localMessage->addition_data; 
  for (int i = 0; i < n; i++) {
    Message* remoteMessage = (Message* )remote[i];
    int remoteSum = remoteMessage->addition_data; 
    localSum += remoteSum; 
  }

  localMessage->addition_data = localSum; 
  return localMessage; 
}

void* mergeMultplication(int *size, void* data, void** remote, int n)
{
  Message* localMessage = (Message* )data; 
  int localProd = localMessage->prod_data; 
  for (int i = 0; i < n; i++) {
    Message* remoteMessage = (Message* )remote[i];
    int remoteProd = remoteMessage->prod_data; 
    localProd *= remoteProd; 
  }

  localMessage->prod_data = localProd; 
  return localMessage; 
}

int factorial(int n) {
  int result = 1;
  for (int i = 2; i <= n; i++) {
      result *= i;
  }
  return result;
}

void ping_handler(void *vmsg)
{
  int n = CmiNumPes() - 1;
  int expectedSum = n * (n + 1) / 2;
  int expectedProd = factorial(n + 1);
  
  // the addition reduction finished 
  if (((Message *)vmsg)->prod_data == 0)
  {
    if (expectedSum != ((Message *)vmsg)->addition_data)
    {
      CmiAbort("Sum Reduction result mismatch: expected %d, got %d\n", expectedSum, ((Message *)vmsg)->addition_data);
    }
  }

  // the multiplication reduction finished
  if (((Message *)vmsg)->addition_data == 0)
  {
    if (expectedProd != ((Message *)vmsg)->prod_data) 
    {
      CmiAbort("Product Reduction result mismatch: expected %d, got %d\n", expectedProd, ((Message *)vmsg)->prod_data);
    }
  } 

  doneCounter++;
  if (doneCounter == 2) {
    // compute expected result:
    printf("Reductions done on %d. Results match up with expected\n", CmiMyPe());
    CmiExit(0);
  }
}

CmiStartFn mymain(int argc, char **argv)
{
  int handlerId = CmiRegisterHandler(ping_handler);

  // create a message
  Message *msg1 = new Message;
  msg1->header.handlerId = handlerId;
  msg1->header.messageSize = sizeof(Message);
  msg1->addition_data = CmiMyPe();
  msg1->prod_data = 0;

  

  Message* msg2 = new Message; 
  msg2->header.handlerId = handlerId; 
  msg2->header.messageSize = sizeof(Message);
  msg2->addition_data = 0; 
  msg2->prod_data = CmiMyPe() + 1; // we are doing + 1, or else we would get 0 as the result everytime


  CmiReduce(msg1, sizeof(Message), mergeAddition);
  CmiReduce(msg2, sizeof(Message), mergeMultplication);
  return 0;
}

int main(int argc, char **argv)
{
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}