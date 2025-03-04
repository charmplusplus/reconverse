#ifndef CONVCORE_H
#define CONVCORE_H

//#include "converse.h"
#include "queue.h"

typedef struct Header
{
    int handlerId;
    int messageId;
    int messageSize;
    int destPE;
} CmiMessageHeader;

#define CmiMsgHeaderSizeBytes sizeof(CmiMessageHeader)

// typedef struct CmiMessageStruct
// {
//     CmiMessageHeader header;
//     char data[];
// } CmiMessage;

void CmiStartThreads(char **argv);
void *converseRunPe(void *arg);

// HANDLERS
// TODO: what is CmiHandlerEx in old converse?

typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr); // ignore for now

void CmiCallHandler(int handlerId, void *msg);

typedef struct HandlerInfo
{
    CmiHandler hdlr;
    void *userPtr;
} CmiHandlerInfo;

std::vector<CmiHandlerInfo> *CmiGetHandlerTable();

/*Cmi Functions*/

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

// message allocation
void *CmiAlloc(int size);
void CmiFree(void *msg);

// message sending
void CmiPushPE(int destPE, int messageSize, void *msg);
void CmiSyncSend(int destPE, int messageSize, void *msg);
void CmiSyncSendAndFree(int destPE, int messageSize, void *msg);

// broadcasts
void CmiSyncBroadcast(int size, void *msg);
void CmiSyncBroadcastAndFree(int size, void *msg);
void CmiSyncBroadcastAll(int size, void *msg);
void CmiSyncBroadcastAllAndFree(int size, void *msg);

// node queue
ConverseQueue<void *> *CmiGetNodeQueue();
void CmiSyncNodeSendAndFree(unsigned int destNode, unsigned int size, void *msg);

int CmiPrintf(const char *format, ...);
int CmiGetArgc(char **argv);

#endif
