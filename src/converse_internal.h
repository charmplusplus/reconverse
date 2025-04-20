//functions declarations internal to converse 

#ifndef CONVCORE_H
#define CONVCORE_H

#include "converse.h"
#include "converse_config.h"
#include "comm_backend/comm_backend.h"
#include "queue.h"

void CmiStartThreads(char **argv);
void converseRunPe(int rank);

// HANDLERS
// TODO: what is CmiHandlerEx in old converse?

typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr); // ignore for now
typedef void * (*CmiReduceMergeFn)(int*, void*, void**, int);

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

typedef struct  
{
    int ReductionID; // ID associated with the reduction. Different reductions will correspond to different IDs
    int numChildren; // number of child PEs/chares in the spanning tree for the reduction
    int messagesReceived; // used to keep track of how many contributions have been received from child chares
    bool localContributed; // flag to indicate if the local PE/chare has contributed to the reduction
    void* localbuffer; // local buffer to store the data
    void* remotebuffer; // remote buffer to store the data
    int parent; // parent PE in the spanning tree
    struct {
      CmiHandler desthandler; // the handler that will process the final result of the reduction 
      CmiReduceMergeFn mergefn; // function used to combine partial results from different PEs into a single result 
    } ops; 
} CmiReduction; 

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


//REDUCTION RELATED FUNCTIONS/DEFINITIONS

using CmiReductionID = std::uint32_t; //is uint32_t good enough?

// defines starting constants for managing reduction IDs. 
// we choose these offsets to avoid conflicts with other IDs in the system
enum : CmiReductionID {
  CmiReductionID_globalOffset = 0, 
  CmiReductionID_requestOffset = 1, 
  CmiReductionID_dynamicOffset = 2, 
  CmiReductionID_multiplier = 4
};

//declares the variables _reduction_global_ID, _reduction_request_ID, and _reduction_dynamic_ID; 
CpvStaticDeclare(CmiReductionID, _reduction_global_ID);
CpvStaticDeclare(CmiReductionID, _reduction_request_ID);
CpvStaticDeclare(CmiReductionID, _reduction_dynamic_ID);


void CmiReductionsInit(void);
CmiReductionID CmiGetNextglobalReductionID(void);
CmiReductionID CmiGetNextRequestReductionID(void);
CmiReductionID CmiGetNextDynamicReductionID(void);

#endif
