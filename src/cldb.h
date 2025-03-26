#include <stdio.h>
#include "converse.h"
#include <pthread.h>
#include <thread>

#define MAXMSGBFRSIZE 100000

extern thread_local int CldHandlerIndex;
extern thread_local int CldNodeHandlerIndex;
extern thread_local int CldBalanceHandlerIndex;
extern thread_local int CldRelocatedMessages;
extern thread_local int CldLoadBalanceMessages;
extern thread_local int CldMessageChunks;

void CldRestoreHandler(char *);
void CldSwitchHandler(char *, int);
void CldModuleGeneralInit(char **);
//void seedBalancerExit(void);