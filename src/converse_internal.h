#ifndef CONVCORE_H
#define CONVCORE_H

#include "converse.h"
#include "converse_config.h"
#include "comm_backend/comm_backend.h"
#include "queue.h"

// typedef struct CmiMessageStruct
// {
//     CmiMessageHeader header;
//     char data[];
// } CmiMessage;

void CmiStartThreads(char **argv);
void converseRunPe(int rank);

// HANDLERS
// TODO: what is CmiHandlerEx in old converse?

void CmiCallHandler(int handlerId, void *msg);

typedef struct HandlerInfo
{
    CmiHandler hdlr;
    void *userPtr;
} CmiHandlerInfo;

std::vector<CmiHandlerInfo> *CmiGetHandlerTable();

typedef struct State
{
    int pe;
    int rank;
    int node;
    ConverseQueue<void *> *queue;
    int stopFlag;

} CmiState;

// state relevant functionality
CmiState *CmiGetState(void);
void CmiInitState(int pe);
ConverseQueue<void *> *CmiGetQueue(int pe);

void CmiPushPE(int destPE, int messageSize, void *msg);

// node queue
ConverseNodeQueue<void *> *CmiGetNodeQueue();

//idle
bool CmiGetIdle();
void CmiSetIdle(bool idle);
double CmiGetIdleTime();
void CmiSetIdleTime(double time);

#endif
