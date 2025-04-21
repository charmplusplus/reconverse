//functions declarations internal to converse 

#ifndef CONVCORE_H
#define CONVCORE_H

#include <cstring>

#include "converse.h"
#include "converse_config.h"
#include "comm_backend/comm_backend.h"
#include "comm_backend/comm_backend_internal.h"
#include "queue.h"

void CmiStartThreads(char **argv);
void converseRunPe(int rank);

// HANDLERS
// TODO: what is CmiHandlerEx in old converse?

typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr); // ignore for now

void CmiCallHandler(int handlerId, void *msg);
void CmiBcastHandler(void *msg);
void CmiNodeBcastHandler(void *msg);
void CmiExitHandlerLocal(void *msg);

typedef struct HandlerInfo
{
    CmiHandler hdlr;
    void *userPtr; //does this point to the mesage data itself 
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
void CrnInit(void);

void CmiPushPE(int destPE, int messageSize, void *msg);

// node queue
ConverseNodeQueue<void *> *CmiGetNodeQueue();

// exit handler function 
void CmiExitHandler(int status);

//idle
bool CmiGetIdle();
void CmiSetIdle(bool idle);
double CmiGetIdleTime();
void CmiSetIdleTime(double time);

//cpu affinity
typedef struct
{
  int num_pus;
  int num_cores;
  int num_sockets;

  int total_num_pus;
} CmiHwlocTopology;

extern CmiHwlocTopology CmiHwlocTopologyLocal;

extern void CmiInitHwlocTopology(void);
extern int  CmiSetCPUAffinity(int);

#endif
