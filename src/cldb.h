#include "converse.h"
#include <pthread.h>
#include <stdio.h>
#include <thread>

#define MAXMSGBFRSIZE 100000

CpvExtern(int, CldHandlerIndex);
extern thread_local int CldNodeHandlerIndex;
extern thread_local int CldBalanceHandlerIndex;
extern thread_local int CldRelocatedMessages;
extern thread_local int CldLoadBalanceMessages;
extern thread_local int CldMessageChunks;

void CldRestoreHandler(char *);
void CldSwitchHandler(char *, int);
void CldModuleGeneralInit(char **);
// void seedBalancerExit(void);

// Token queue: stealable work items placed in the scheduler queue.
// CldPutToken enqueues msg so it can be stolen; CldGetToken extracts
// one for sending to another PE; CldCountTokens returns the current count.
void CldPutToken(char *msg);
void CldGetToken(char **msg);
int CldCountTokens();

// Called by the active strategy after a token is processed (load decreased).
// Each strategy file must provide its own definition.
void LoadNotifyFn(int load);