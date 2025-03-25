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
extern thread_local int CldLoadNotify;
extern thread_local CmiNodeLock cldLock;

void CldMultipleSend(int pe, int numToSend, int rank, int immed);
void CldSimpleMultipleSend(int pe, int numToSend, int rank);
void CldSetPEBitVector(const char *);

int  CldLoad(void);
int  CldLoadRank(int rank);
int  CldCountTokens(void);
int  CldCountTokensRank(int rank);
void CldPutToken(char *);
void CldPutTokenPrio(char *);
void CldRestoreHandler(char *);
void CldSwitchHandler(char *, int);
void CldModuleGeneralInit(char **);
int  CldPresentPE(int pe);
void seedBalancerExit(void);