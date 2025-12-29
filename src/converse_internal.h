// functions declarations internal to converse

#ifndef CONVCORE_H
#define CONVCORE_H

#include <cstring>
#include "converse.h"

#include "converse.h"
#include "converse_config.h"
#include "queue.h"

#include "queue.h"

#include "comm_backend/comm_backend.h"
#include "comm_backend/comm_backend_internal.h"

typedef struct GroupDef_s {
  CmiMessageHeader core;
  struct GroupDef_s *next;
  CmiGroup group;
  int npes;
  int pes[1];
} *GroupDef;

#define GROUPTAB_SIZE 101

//debug
#define  DEBUGF(...)    //CmiPrintf(__VA_ARGS__)

void CmiStartThreads(char **argv);
void converseRunPe(int rank, int everReturn);
void collectiveInit(void);

// HANDLERS
// TODO: what is CmiHandlerEx in old converse?

void CmiCallHandler(int handlerId, void *msg);
void CmiBcastHandler(void *msg);
void CmiNodeBcastHandler(void *msg);
void CmiExitHandler(void *msg);
void CmiGroupHandler(void *msg);
void CmiReduceHandler(void *msg);

typedef struct HandlerInfo {
  union{
    CmiHandler hdlr; // handler function
    CmiHandlerEx exhdlr; // handler function with user pointer
  };
  void *userPtr; // does this point to the mesage data itself
} CmiHandlerInfo;

std::vector<CmiHandlerInfo> *CmiGetHandlerTable();

typedef struct State {
  int pe = 0;
  int rank = 0;
  int node = 0;
  ConverseQueue<void *> *queue = nullptr;
  int stopFlag = 0;
} CmiState;

extern int backend_poll_freq; // poll every backend_poll_freq iterations of the
                             // scheduler loop
extern int backend_poll_thread; // every backend_poll_thread threads will call progress

// state relevant functionality
CmiState *CmiGetState(void);
void CmiInitState(int pe);
ConverseQueue<void *> *CmiGetQueue(int pe);
void CrnInit(void);

void CmiPushPE(int destPE, int messageSize, void *msg);

// node queue
ConverseNodeQueue<void *> *CmiGetNodeQueue();

// idle
bool CmiGetIdle();
void CmiSetIdle(bool idle);
double CmiGetIdleTime();
void CmiSetIdleTime(double time);

// cpu affinity
typedef struct {
  int num_pus;
  int num_cores;
  int num_sockets;

  int total_num_pus;
} CmiHwlocTopology;

extern CmiHwlocTopology CmiHwlocTopologyLocal;

extern void CmiInitHwlocTopology(void);
extern int CmiSetCPUAffinity(int);

// REDUCTION RELATED FUNCTIONS/DEFINITIONS

#define CMI_REDUCTION_ID_MULTIPLIER 4

using CmiReductionID =
    decltype(CmiMessageHeader::collectiveMetaInfo); // needs to match header
using CmiBroadcastSource =
    decltype(CmiMessageHeader::collectiveMetaInfo); // needs to match header
typedef struct {
  int ReductionID; // ID associated with the reduction. Different reductions
                   // will correspond to different IDs
  int numChildren; // number of child PEs/chares in the spanning tree for the
                   // reduction
  int messagesReceived;  // used to keep track of how many contributions have
                         // been received from child chares
  bool localContributed; // flag to indicate if the local PE/chare has
                         // contributed to the reduction
  void *localbuffer;     // local buffer to store the data
  int localbufferSize;   // size of the local buffer
  void **remotebuffer;   // remote buffer to store the data
  int parent;            // parent PE in the spanning tree
  struct {
    CmiHandler desthandler; // the handler that will process the final result of
                            // the reduction
    CmiReduceMergeFn mergefn; // function used to combine partial results from
                              // different PEs into a single result
  } ops;
} CmiReduction;

typedef struct {
  // in non-SMP, node reduction is equivalent to PE reduction
  // Reconverse assumes SMP, use a node lock

  CmiNodeLock lock;

  CmiReduction *red;
} CmiNodeReduction;

CpvStaticDeclare(CmiReductionID, _reduction_counter);
CpvStaticDeclare(CmiReduction **,
                 _reduction_info); // an array of pointers to reduction structs
CpvStaticDeclare(CmiReductionID, _reduce_seqID_global);

// atomics used here to support SMP
using CmiNodeReductionID = std::atomic<CmiReductionID>;

CsvStaticDeclare(CmiNodeReductionID, _node_reduction_counter);
CsvStaticDeclare(
    CmiNodeReduction *,
    _node_reduction_info); // an array of pointers to reduction structs

void CmiReductionsInit(void);

// helper function to get the next reduction ID
CmiReductionID CmiGetNextReductionID();

// helper function to get the index into the reduction table for a specific
// reduction ID
unsigned CmiGetReductionIndex(CmiReductionID id);

static CmiReduction *CmiGetCreateReduction(CmiReductionID id);
static void CmiClearReduction(CmiReductionID id);
static void CmiClearNodeReduction(CmiReductionID id);

void CmiInternalReduce(void *msg, int size, CmiReduceMergeFn mergeFn,
                       CmiReduction *red);
void CmiSendReduce(CmiReduction *red);

// internal node reduction functions

CmiReductionID CmiGetNextNodeReductionID();
static CmiReduction *CmiGetCreateNodeReduction(CmiReductionID id);
void CmiInternalNodeReduce(void *msg, int size, CmiReduceMergeFn mergeFn,
                           CmiReduction *red);
void CmiSendNodeReduce(CmiReduction *red);
void CmiNodeReduceHandler(void *msg);

// helpers to get and set red ID in a message
CmiReductionID CmiGetRedID(void *msg);
void CmiSetRedID(void *msg, CmiReductionID redID);

// helpers for broadcast
void CmiSetBcastSource(void *msg, CmiBroadcastSource source);
CmiBroadcastSource CmiGetBcastSource(void *msg);

// helpers for RDMA
void RDMAInit(char **argv);

#endif
