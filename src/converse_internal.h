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

void CmiCallHandler(int handlerId, void *msg);
void CmiBcastHandler(void *msg);
void CmiNodeBcastHandler(void *msg);
void CmiExitHandlerLocal(void *msg);
void CmiReduceHandler(void *msg);

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


//REDUCTION RELATED FUNCTIONS/DEFINITIONS

#define CMI_REDUCTION_ID_MULTIPLIER 4

using CmiReductionID = decltype(CmiMessageHeader::collectiveMetaInfo); // needs to match header
typedef struct  
{
  CmiReductionID ReductionID; // ID associated with the reduction. Different reductions will correspond to different IDs
      CmiReduceMergeFn mergefn; // function used to combine partial results from different PEs into a single result 
    } ops; 
} CmiReduction; 

// defines starting constants for managing reduction IDs. 
// we choose these offsets to avoid conflicts with other IDs in the system
typedef enum {
  globalReduction = 0, 
  requestReduction = 1, 
  dynamicReduction = 2, 
} CmiReductionCategory;


CpvStaticDeclare(CmiReductionID*, _reduction_IDs);
CpvStaticDeclare(CmiReduction**, _reduction_info); //holds pointers to arrays of CmiReduction structs(AKA the reduction table for that PE)

void CmiReductionsInit(void);

// helper function to get the next reduction ID
CmiReductionID CmiGetNextReductionID(CmiReductionCategory category);

// helper function to get the index into the reduction table for a specific reduction ID
unsigned CmiGetReductionIndex(CmiReductionID id, CmiReductionCategory category);

static CmiReduction *CmiGetCreateReduction(CmiReductionID id, CmiReductionCategory category);
static void CmiClearReduction(CmiReductionID id, CmiReductionCategory category);
void CmiGlobalReduce(void *msg, int size, CmiReduceMergeFn mergeFn, CmiReduction *red);
void CmiSendReduce(CmiReduction *red);

// helpers to get and set red ID in a message
CmiReductionID CmiGetRedID(void *msg);
void CmiSetRedID(void *msg, CmiReductionID redID);

#endif
