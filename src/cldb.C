
#include <stdlib.h>
//#include "queueing.h"
#include "cldb.h"
#include <math.h>

typedef char *BitVector;

/*
CpvDeclare(int, CldHandlerIndex);
CpvDeclare(int, CldNodeHandlerIndex);
CpvDeclare(BitVector, CldPEBitVector);
CpvDeclare(int, CldBalanceHandlerIndex);

CpvDeclare(int, CldRelocatedMessages);
CpvDeclare(int, CldLoadBalanceMessages);
CpvDeclare(int, CldMessageChunks);
CpvDeclare(int, CldLoadNotify);

CpvDeclare(CmiNodeLock, cldLock);
*/

thread_local int CldHandlerIndex;
thread_local int CldNodeHandlerIndex;
thread_local int CldBalanceHandlerIndex;
thread_local int CldRelocatedMessages;
thread_local int CldLoadBalanceMessages;
thread_local int CldMessageChunks;

extern void LoadNotifyFn(int);

/* Estimator stuff.  Of any use? */
/*
CpvStaticDeclare(CldEstimatorTable, _estfns);
*/
void CldRegisterEstimator(CldEstimator fn)
{
  /*CpvAccess(_estfns).fns[CpvAccess(_estfns).count++] = fn;*/
}

/* 
int CldEstimate(void)
{
  CldEstimatorTable *estab = &(CpvAccess(_estfns));
  int i, load=0;
  for(i=0; i<estab->count; i++)
    load += (*(estab->fns[i]))();
  return load;
}

static int CsdEstimator(void)
{
  return CsdLength();
}
*/

int CldRegisterInfoFn(CldInfoFn fn)
{
  return CmiRegisterHandler((CmiHandler)fn);
}

int CldRegisterPackFn(CldPackFn fn)
{
  return CmiRegisterHandler((CmiHandler)fn);
}

/* CldSwitchHandler takes a message and a new handler number.  It
 * changes the handler number to the new handler number and move the
 * old to the Xhandler part of the header.  When the message gets
 * handled, the handler should call CldRestoreHandler to put the old
 * handler back.
 *
 * CldPutToken puts a message in the scheduler queue in such a way
 * that it can be retreived from the queue.  Once the message gets
 * handled, it can no longer be retreived.  CldGetToken removes a
 * message that was placed in the scheduler queue in this way.
 * CldCountTokens tells you how many tokens are currently retreivable.  
*/

void CldSwitchHandler(char *cmsg, int handler)
{
  CmiSetXHandler(cmsg, CmiGetHandler(cmsg)); //probably get rid of this in charm
  CmiSetHandler(cmsg, handler);
}

void CldRestoreHandler(char *cmsg)
{
  CmiSetHandler(cmsg, CmiGetXHandler(cmsg));
}

void CldHandler(char *);


void CldModuleGeneralInit(char **argv)
{

}


/*
don't really need now but may want to turn this back on later
void seedBalancerExit(void)
{
  if (_cldb_cs)
    CmiPrintf("[%d] Relocate message number is %d\n", CmiMyPe(), CpvAccess(CldRelocatedMessages));
}
*/