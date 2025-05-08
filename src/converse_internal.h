// functions declarations internal to converse

#ifndef CONVCORE_H
#define CONVCORE_H

#include <cstring>

#include "converse.h"
#include "converse_config.h"
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

void CmiStartThreads(char **argv);
void converseRunPe(int rank);
void collectiveInit(void);

// HANDLERS
// TODO: what is CmiHandlerEx in old converse?

void CmiCallHandler(int handlerId, void *msg);
void CmiBcastHandler(void *msg);
void CmiNodeBcastHandler(void *msg);
void CmiExitHandler(void *msg);
void CmiGroupHandler(void *msg);
void CmiReduceHandler(void *msg);
void CmiUndefinedHandler(void *msg);

typedef struct HandlerInfo {
  union{
    CmiHandler hdlr; // handler function
    CmiHandlerEx exhdlr; // handler function with user pointer
  };
  void *userPtr; // does this point to the mesage data itself
} CmiHandlerInfo;

std::vector<CmiHandlerInfo> *CmiGetHandlerTable();

typedef struct State {
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
#ifdef CMK_SMP
  CmiNodeLock lock;
#endif
  CmiReduction *red;
} CmiNodeReduction;

CpvStaticDeclare(CmiReductionID, _reduction_counter);
CpvStaticDeclare(CmiReduction **,
                 _reduction_info); // an array of pointers to reduction structs
CpvStaticDeclare(CmiReductionID, _reduce_seqID_global);

#ifdef CMK_SMP
using CmiNodeReductionID = std::atomic<CmiReductionID>;
#else
using CmiNodeReductionID = CmiReductionID;
#endif
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


// TASK QUEUE RELATED FUNCTIONS/DEFINITIONS
/**
 * I think we should just abort if we see estimated queue size is more than (say) half of the max size. 
 * I say half because thats a power of 2. But may be a small number will do. 
 * Like estimatedSize > Maxsize - 2*numPEs.  (here estimated size is something like tail-head (or vice versa?). 
 * And note not to use atomic etc. for accessing tail and head for this estimated size
 */

/**
 * I suggest we increase the taskq size, document it (and maybe provide a build option or just instructions 
 * for where to change it in the doc), and see if a warning (if not abort) is possible. 
 * Note that the head and tail roll over (as 32 bit ints.. It probably should be unsigned int) 
 * (and the actual index is calculated as (head % qsize). So the difference will be negative briefly 
 * when the tail rolls over after going above MAXINT but head hasn’t yet.. but thats ok. 
 * The purpose of the warning (or abort) is still reasonably served
 */

/**
 * For later, we should consider unbounded queue. But it won’t be as efficient as this. 
 * (one change I suggest in the current code is that queuesize be specified as a power of two 
 * (by specifying the exponent), so there is no chance of someone changing it to a non power of 2, 
 * and then changing the modulo operator to a bitmas
 */
#define TASKQUEUE_SIZE 2048 // in old converse it is 1024

#ifdef CMK_SMP
#define CmiMemoryWriteFence() __sync_synchronize() // Memory fence for SMP mode
#else
#define CmiMemoryWriteFence() // No-op if not in SMP mode
#endif

typedef int taskq_idx;

typedef struct TaskQueueStruct {
  taskq_idx head; // This pointer indicates the first task in the queue
  taskq_idx tail; // The tail indicates the array element next to the last available task in the queue. So, if head == tail, the queue is empty
  void *data[TASKQUEUE_SIZE];
} TaskQueue;

//CsvStaticDeclare(TaskQueue**, task_q);

TaskQueue* TaskQueueCreate();
void TaskQueuePush(TaskQueue* queue, void* data);
void* TaskQueuePop(TaskQueue* queue);
void* TaskQueueSteal(TaskQueue* queue);
void TaskQueueDestroy(TaskQueue* queue);
void TaskStealBeginIdle(void);

void StealTask(void);
// void CmiTaskQueueInit(void);


extern ConverseQueue<void *> **Cmi_queues; // array of queue pointers
extern TaskQueue** Cmi_taskqueues; 

#endif