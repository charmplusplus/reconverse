//+pe <N> threads, each running a scheduler
#include "barrier.h"
#include "converse_internal.h"
#include "queue.h"
#include "scheduler.h"

#include <cinttypes>
#include <cstdarg>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>

static constexpr unsigned int CmiLogMaxReductions = 4u;
static constexpr int CmiMaxReductions = 1u << CmiLogMaxReductions;
static constexpr int ReductionSegmentSize = CmiMaxReductions / 3;

// GLOBALS
int Cmi_argc;
static char **Cmi_argv;
int Cmi_npes;   // total number of PE's across the entire system
int Cmi_nranks; // TODO: this isnt used in old converse, but we need to know how
                // many PEs are on our node?
int Cmi_mynode;
int Cmi_mynodesize; // represents the number of PE's/threads on a single
                    // physical node. In SMP mode, each PE is run by a seperate
                    // thread, so Cmi_nodesize in that case represents the
                    // number of threads (PEs) on that machine
int Cmi_numnodes;   // represents the number of physical nodes/systems machine
int Cmi_nodestart;
std::vector<CmiHandlerInfo> **CmiHandlerTable; // array of handler vectors
ConverseNodeQueue<void *> *CmiNodeQueue;
double Cmi_startTime;
CmiSpanningTreeInfo *_topoTree = NULL;

void CldModuleInit(char **);

// PE LOCALS that need global access sometimes
static ConverseQueue<void *> **Cmi_queues; // array of queue pointers

// PE LOCALS
thread_local int Cmi_myrank;
thread_local CmiState *Cmi_state;
thread_local bool idle_condition;
thread_local double idle_time;
thread_local int CmiGroupCounter;
thread_local int CmiGroupHandlerIndex;
thread_local GroupDef *CmiGroupTable;

// Special operation handlers (TODO: should these be special values instead like
// the exit handler)
int Cmi_bcastHandler;
int Cmi_nodeBcastHandler;
int Cmi_exitHandler;
int Cmi_reduceHandler;
int Cmi_multicastHandler;

// TODO: padding for all these thread_locals and cmistates?

comm_backend::AmHandler AmHandlerPE;
comm_backend::AmHandler AmHandlerNode;

void CommLocalHandler(comm_backend::Status status) { CmiFree(status.msg); }

void CommRemoteHandlerPE(comm_backend::Status status) {
  CmiMessageHeader *header = (CmiMessageHeader *)status.msg;
  int destPE = header->destPE;
  CmiPushPE(destPE, status.size, status.msg);
}

void CommRemoteHandlerNode(comm_backend::Status status) {
  CmiNodeQueue->push(status.msg);
}

void CmiCallHandler(int handler, void *msg) {
  CmiGetHandlerTable()->at(handler).hdlr(msg);
}

void converseRunPe(int rank) {
  // init state
  CmiInitState(rank);

  // init things like cld module, ccs, etc
  CldModuleInit(Cmi_argv);
#ifdef SET_CPU_AFFINITY
  CmiSetCPUAffinity(rank);
#endif

  // register special operation handlers
  Cmi_bcastHandler = CmiRegisterHandler(CmiBcastHandler);
  Cmi_nodeBcastHandler = CmiRegisterHandler(CmiNodeBcastHandler);
  Cmi_exitHandler = CmiRegisterHandler(CmiExitHandler);
  CmiGroupHandlerIndex = CmiRegisterHandler(CmiGroupHandler);
  Cmi_reduceHandler = CmiRegisterHandler(CmiReduceHandler);
  // Cmi_multicastHandler = CmiRegisterHandler(CmiMulticastHandler);

  CmiGroupTable = (GroupDef *)calloc(GROUPTAB_SIZE, sizeof(GroupDef));
  CmiReductionsInit();

  // barrier to ensure all global structs are initialized
  CmiNodeBarrier();

  CthInit(NULL);
  CthSchedInit();

  // call initial function and start scheduler
  Cmi_startfn(Cmi_argc, Cmi_argv);
  CsdScheduler();
}

void CmiStartThreads() {
  // allocate global arrayss
  Cmi_queues = new ConverseQueue<void *> *[Cmi_mynodesize];
  CmiHandlerTable = new std::vector<CmiHandlerInfo> *[Cmi_mynodesize];
  CmiNodeQueue = new ConverseNodeQueue<void *>();

  std::vector<std::thread> threads;
  for (int i = 0; i < Cmi_mynodesize; i++) {
    std::thread t(converseRunPe, i);
    threads.push_back(std::move(t));
  }

  for (auto &thread : threads) {
    thread.join();
  }
  delete[] Cmi_queues;
  delete CmiNodeQueue;
  delete[] CmiHandlerTable;
  Cmi_queues = nullptr;
  CmiNodeQueue = nullptr;
  CmiHandlerTable = nullptr;
}

// argument form: ./prog +pe <N>
// TODO: this function need error checking
// TODO: the input parsing, cmi_arg parsing is not done/robust
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched,
                  int initret) {

  Cmi_startTime = getCurrentTime();

  Cmi_npes = atoi(argv[2]);
  // int plusPSet = CmiGetArgInt(argv,"+pe",&Cmi_npes);

  Cmi_argc = argc - 2; // TODO: Cmi_argc doesn't include runtime args?
  Cmi_argv = (char **)malloc(sizeof(char *) * (argc + 1));
  int i;
  for (i = 2; i <= argc; i++)
    Cmi_argv[i - 2] = argv[i];

  comm_backend::init(&argc, &Cmi_argv);
  Cmi_mynode = comm_backend::getMyNodeId();
  Cmi_numnodes = comm_backend::getNumNodes();
  if (Cmi_mynode == 0)
    printf("Charm++> Running in SMP mode on %d nodes and %d PEs\n",
           Cmi_numnodes, Cmi_npes);
  // Need to discuss this with the team
  if (Cmi_npes < Cmi_numnodes) {
    fprintf(stderr, "Error: Number of PEs must be greater than or equal to "
                    "number of nodes\n");
    exit(1);
  }
  if (Cmi_npes % Cmi_numnodes != 0) {
    fprintf(stderr,
            "Error: Number of PEs must be a multiple of number of nodes\n");
    exit(1);
  }
  Cmi_mynodesize = Cmi_npes / Cmi_numnodes;
  Cmi_nodestart = Cmi_mynode * Cmi_mynodesize;
  // register am handlers
  AmHandlerPE = comm_backend::registerAmHandler(CommRemoteHandlerPE);
  AmHandlerNode = comm_backend::registerAmHandler(CommRemoteHandlerNode);

#ifdef SET_CPU_AFFINITY
  CmiInitHwlocTopology();
#endif

  Cmi_startfn = fn;

  CmiStartThreads();
  free(Cmi_argv);

  comm_backend::exit();
}

// CMI STATE
CmiState *CmiGetState(void) { return Cmi_state; };

void CmiInitState(int rank) {
  // allocate state
  Cmi_state = new CmiState;
  Cmi_state->pe = Cmi_nodestart + rank;
  Cmi_state->rank = rank;
  Cmi_state->node = Cmi_mynode;
  Cmi_state->stopFlag = 0;

  Cmi_myrank = rank;

  // group inits
  CmiGroupCounter = 0;

  // initialize idle state
  CmiSetIdle(false);
  CmiSetIdleTime(0.0);

  // allocate global entries
  ConverseQueue<void *> *queue = new ConverseQueue<void *>();
  std::vector<CmiHandlerInfo> *handlerTable = new std::vector<CmiHandlerInfo>();

  Cmi_queues[Cmi_myrank] = queue;
  CmiHandlerTable[Cmi_myrank] = handlerTable;

  // random
  CrnInit();

  CcdModuleInit();
}

ConverseQueue<void *> *CmiGetQueue(int rank) { return Cmi_queues[rank]; }

int CmiMyRank() { return CmiGetState()->rank; }

int CmiMyPe() { return CmiGetState()->pe; }

int CmiStopFlag() { return CmiGetState()->stopFlag; }

int CmiMyNode() { return CmiGetState()->node; }

int CmiMyNodeSize() { return Cmi_mynodesize; }

int CmiNumNodes() { return Cmi_numnodes; }

int CmiNumPes() { return Cmi_npes; }

int CmiNodeOf(int pe) { return pe / Cmi_mynodesize; }

int CmiRankOf(int pe) { return pe % Cmi_mynodesize; }

int CmiNodeFirst(int node) { return node * Cmi_mynodesize; }

std::vector<CmiHandlerInfo> *CmiGetHandlerTable() {
  return CmiHandlerTable[CmiMyRank()];
}

CmiHandler CmiHandlerToFunction(int handlerId) {
  return CmiGetHandlerTable()->at(handlerId).hdlr;
}

int CmiGetInfo(void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  return header->collectiveMetaInfo;
}

void CmiSetInfo(void *msg, int infofn) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->collectiveMetaInfo = infofn;
}

void CmiPushPE(int destPE, int messageSize, void *msg) {
  int rank = CmiRankOf(destPE);
  CmiAssertMsg(
      rank >= 0 && rank < Cmi_mynodesize,
      "CmiPushPE(myPe: %d, destPe: %d, nodeSize: %d): rank out of range",
      CmiMyPe(), destPE, Cmi_mynodesize);
  Cmi_queues[rank]->push(msg);
}

void *CmiAlloc(int size) {
  if (size <= 0) {
    CmiPrintf("CmiAlloc: size <= 0\n");
    return nullptr;
  }
  return malloc(size);
}

void CmiFree(void *msg) {
  if (msg == nullptr) {
    CmiPrintf("CmiFree: msg is nullptr\n");
    return;
  }
  free(msg);
}

void CmiSyncSend(int destPE, int messageSize, void *msg) {
  char *copymsg = (char *)CmiAlloc(messageSize);
  std::memcpy(copymsg, msg,
              messageSize); // optionally avoid memcpy and block instead
  CmiSyncSendAndFree(destPE, messageSize, copymsg);
}

void CmiSyncSendAndFree(int destPE, int messageSize, void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);

  header->destPE = destPE;
  int destNode = CmiNodeOf(destPE);

  if (destNode >= Cmi_numnodes || destNode < 0) {
    CmiAbort("Destnode %d out of range %d\n", destPE, Cmi_numnodes);
  }

  if (CmiMyNode() == destNode) {
    CmiPushPE(destPE, messageSize, msg);
  } else {
    comm_backend::sendAm(destNode, msg, messageSize, comm_backend::MR_NULL,
                         CommLocalHandler,
                         AmHandlerPE); // Commlocalhandler will free msg
  }
}

/* Broadcast to everyone but the source pe. Source does not free. */
void CmiSyncBroadcast(int size, void *msg) {
  int pe = CmiMyPe();

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#ifdef SPANTREE
  CmiSetBcastSource(msg, pe); // used to skip the source
  header->swapHandlerId = header->handlerId;
  header->handlerId = Cmi_bcastHandler;
  CmiSyncSend(0, size, msg);
#else

  for (int i = pe + 1; i < Cmi_npes; i++)
    CmiSyncSend(i, size, msg);

  for (int i = 0; i < pe; i++)
    CmiSyncSend(i, size, msg);
#endif
}

void CmiSyncBroadcastAndFree(int size, void *msg) {
  CmiSyncBroadcast(size, msg);
  CmiFree(msg);
}

void CmiSyncBroadcastAll(int size, void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#ifdef SPANTREE
  CmiSetBcastSource(msg, -1); // don't skip the source
  header->swapHandlerId = header->handlerId;

  header->handlerId = Cmi_bcastHandler;
  CmiSyncSend(0, size, msg);
#else
  for (int i = 0; i < Cmi_npes; i++)
    CmiSyncSend(i, size, msg);
#endif
}

void CmiSyncBroadcastAllAndFree(int size, void *msg) {
  CmiSyncBroadcastAll(size, msg);
  CmiFree(msg);
}

void CmiWithinNodeBroadcast(int size, void *msg) {
  for (int i = 0; i < Cmi_mynodesize; i++) {
    int destPe = CmiMyNode() * Cmi_mynodesize + i;
    CmiSyncSend(destPe, size, msg);
  }
}

void CmiSyncNodeBroadcast(unsigned int size, void *msg) {
  int node = CmiMyNode();

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#ifdef SPANTREE
  CmiSetBcastSource(msg, node); // used to skip the source
  header->swapHandlerId = header->handlerId;
  header->handlerId = Cmi_nodeBcastHandler;
  CmiSyncNodeSend(0, size, msg);
#else

  for (int i = node + 1; i < Cmi_numnodes; i++)
    CmiSyncNodeSend(i, size, msg);

  for (int i = 0; i < node; i++)
    CmiSyncNodeSend(i, size, msg);
#endif
}

void CmiSyncNodeBroadcastAndFree(unsigned int size, void *msg) {
  CmiSyncNodeBroadcast(size, msg);
  CmiFree(msg);
}

void CmiSyncNodeBroadcastAll(unsigned int size, void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#ifdef SPANTREE
  CmiSetBcastSource(msg, -1); // don't skip the source
  header->swapHandlerId = header->handlerId;
  header->handlerId = Cmi_nodeBcastHandler;
  CmiSyncNodeSend(0, size, msg);
#else

  for (int i = 0; i < Cmi_numnodes; i++)
    CmiSyncNodeSend(i, size, msg);
#endif
}

void CmiSyncNodeBroadcastAllAndFree(unsigned int size, void *msg) {
  CmiSyncNodeBroadcastAll(size, msg);
  CmiFree(msg);
}

/* Handler for broadcast via the spanning tree. */
void CmiBcastHandler(void *msg) {
  int mype = CmiMyPe();
  int numChildren = CmiNumSpanTreeChildren(mype);
  int children[numChildren];
  CmiSpanTreeChildren(mype, children);

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);

  // send broadcast to all children
  for (int i = 0; i < numChildren; i++) {
    CmiSyncSend(children[i], header->messageSize, msg);
  }

  // call handler locally (unless I am source of broadcast, and bcast is
  // exclusive)
  if (CmiGetBcastSource(msg) != mype) {
    CmiCallHandler(header->swapHandlerId, msg);
  }
}

/* Handler for node broadcast via the spanning tree. */
void CmiNodeBcastHandler(void *msg) {
  int mynode = CmiMyNode();
  int numChildren = CmiNumNodeSpanTreeChildren(mynode);
  int children[numChildren];
  CmiNodeSpanTreeChildren(mynode, children);

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);

  // send broadcast to node children
  for (int i = 0; i < numChildren; i++) {
    CmiSyncNodeSend(children[i], header->messageSize, msg);
  }

  if (CmiGetBcastSource(msg) != mynode) {
    CmiCallHandler(header->swapHandlerId, msg);
  }
}

void CmiSetBcastSource(void *msg, CmiBroadcastSource source) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->collectiveMetaInfo = source;
}

CmiBroadcastSource CmiGetBcastSource(void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  return header->collectiveMetaInfo;
}

// EXIT TOOLS

void CmiExitHelper(int status) {
  CmiMessageHeader *exitMsg = new CmiMessageHeader(); // might need to allocate
  exitMsg->handlerId = Cmi_exitHandler;
  CmiSyncBroadcastAllAndFree(sizeof(*exitMsg), exitMsg);
}

void CmiExit(int status) // note: status isn't being used meaningfully
{
  CmiExitHelper(status);
}

// REDUCTION TOOLS/FUNCTIONS

// each pe/thread will call this function?
void CmiReductionsInit(void) {
  CpvInitialize(CmiReduction **, _reduction_info);
  CpvInitialize(CmiReductionID, _reduction_counter);

  // allocating memory for reduction info table
  auto redinfo =
      (CmiReduction **)malloc(CmiMaxReductions * sizeof(CmiReduction *));
  for (int i = 0; i < CmiMaxReductions; ++i)
    redinfo[i] = NULL;
  CpvAccess(_reduction_info) = redinfo;

  // each pe will have its own reduction counter, and sets it to 0
  // how do we ensure that 2 threads who call reductions at the same time don't
  // get the same ID again?
  CpvAccess(_reduction_counter) = 0;
}

static inline CmiReductionID getNextID(CmiReductionID &ctr) {
  CmiReductionID old = ctr;
  CmiReductionID next = old + 1;
  if (next < old) {
    next = 0;
  }
  ctr = next;
  return old;
}

// discussed that we don't want to use atomic here, since we would have to pass
// in something different. Ideas?
#if CMK_SMP
static inline CmiReductionID getNextID(std::atomic<CmiReductionID> &ctr) {
  CmiReductionID old =
      ctr.fetch_add(1, std::memory_order_relaxed); // Increment atomically
  CmiReductionID next = old + 1;
  if (next < old) {                          // Check for overflow
    ctr.store(0, std::memory_order_relaxed); // Reset to 0 on overflow
  }
  return old;
}
#endif

CmiReductionID CmiGetNextReductionID() {
  return getNextID(CpvAccess(_reduction_counter));
}

// treating the id as the index into the reduction table
// utilized in getCreateReduction and clearReduction to find the reduction
// struct associated with an ID
unsigned CmiGetReductionIndex(CmiReductionID id) {
  if (id >= CmiMaxReductions) {
    CmiAbort("CmiGetReductionIndex: id >= CmiMaxReductions");
  }

  return id;
}

static CmiReduction *CmiGetCreateReduction(CmiReductionID id) {
  // should handle the 2 cases:
  // 1. a reduction message arrives from a child for ex), but the parent hasn't
  // gotten the chance to create a reduction struct yet
  // 2. a reduction structure already exists adn teh parent has initiaited its
  // own contribution
  auto &reduction_ref = CpvAccess(_reduction_info)[CmiGetReductionIndex(id)];
  CmiReduction *red = reduction_ref;
  if (reduction_ref == NULL) {
    CmiReduction *newred = (CmiReduction *)malloc(sizeof(CmiReduction));
    newred->ReductionID = id;

    int mype = CmiMyPe();
    newred->numChildren = CmiNumSpanTreeChildren(mype);
    newred->parent = CmiSpanTreeParent(mype);

    newred->messagesReceived = 0;
    newred->localContributed = false;
    newred->localbuffer = NULL;
    newred->localbufferSize = 0;
    newred->remotebuffer =
        (void **)malloc(newred->numChildren * sizeof(void *));

    newred->ops.desthandler = NULL;
    newred->ops.mergefn = NULL;

    red = newred;
    reduction_ref = newred;
  }

  return red;
}

static void CmiClearReduction(CmiReductionID id) {
  auto &reduction_ref = CpvAccess(_reduction_info)[CmiGetReductionIndex(id)];
  CmiReduction *red = reduction_ref;
  if (red != NULL) {
    free(red->remotebuffer);
    // we assume the user is freeing the actual messages that the buffer is
    // holding or is that something we do?
    free(red);
  }
  reduction_ref = NULL;
}

// gets called by every PE pariticapting in the reduction
void CmiReduce(void *msg, int size, CmiReduceMergeFn mergeFn) {
  const CmiReductionID id = CmiGetNextReductionID();
  CmiReduction *red = CmiGetCreateReduction(id);
  CmiInternalReduce(msg, size, mergeFn, red);
}

// assuming the void* msg is dynamically allocated
void CmiInternalReduce(void *msg, int size, CmiReduceMergeFn mergeFn,
                       CmiReduction *red) {
  red->localContributed = true;
  red->localbuffer = msg;
  red->localbufferSize = size;

  red->ops.desthandler = (CmiHandler)CmiGetHandlerFunction(msg);
  red->ops.mergefn = mergeFn;
  CmiSendReduce(red);
}

void CmiSendReduce(CmiReduction *red) {

  // if we haven't received all the messages yet, we can't send the reduction up
  // to the parent
  if (!red->localContributed || red->numChildren != red->messagesReceived) {
    return;
  }

  // anything below this assumes that we are either a leaf node or a nonleaf
  // node but have received all our messages
  void *mergedData = red->localbuffer;
  int msg_size = red->localbufferSize;

  // if we are an nonleaf node, we need to merge the data from our children
  if (red->numChildren > 0) {
    mergedData = (red->ops.mergefn)(&msg_size, red->localbuffer,
                                    red->remotebuffer, red->numChildren);
  }
  void *msg = mergedData;
  // if we have a parent
  if (red->parent != -1) {
    // dont think we need this part anymore because we register this in the
    // beginning
    CmiSetHandler(msg, Cmi_reduceHandler);

    // we need metadata to store the reduction ID itself?
    // because we need to know that a) its a reduction message and b) what
    // reduction ID it is
    CmiSetRedID(msg, red->ReductionID);

    CmiSyncSendAndFree(red->parent, msg_size, msg);
  } else {
    (red->ops.desthandler)(msg);
  }
  CmiClearReduction(red->ReductionID);
}

void CmiReduceHandler(void *msg) {
  CmiReduction *reduction = CmiGetCreateReduction(CmiGetRedID(msg));

  // how are we ensuring the messages arrive in order again?
  reduction->remotebuffer[reduction->messagesReceived] = (char *)msg;
  reduction->messagesReceived++;
  CmiSendReduce(reduction);
}

// extract reduction ID from message
CmiReductionID CmiGetRedID(void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  return header->collectiveMetaInfo;
}

// set reduction ID for a reduction message
void CmiSetRedID(void *msg, CmiReductionID redID) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->collectiveMetaInfo = redID;
}

// HANDLER TOOLS
int CmiRegisterHandler(CmiHandler h) {
  // add handler to vector
  std::vector<CmiHandlerInfo> *handlerVector = CmiGetHandlerTable();

  handlerVector->push_back({h, nullptr});
  return handlerVector->size() - 1;
}

void CmiNodeBarrier(void) {
  static Barrier nodeBarrier(CmiMyNodeSize());
  nodeBarrier.wait(); // TODO: this may be broken...
}

// TODO: in the original converse, this variant blocks comm thread as well.
// CmiNodeBarrier does not.
void CmiNodeAllBarrier() {
  static Barrier nodeBarrier(CmiMyNodeSize());
  nodeBarrier.wait();
}

// status default is 0
void CsdExitScheduler() { CmiGetState()->stopFlag = 1; }

void CmiExitHandler(void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  int status = header->collectiveMetaInfo;

  if (status == 1)
    abort();

  CsdExitScheduler();
}

ConverseNodeQueue<void *> *CmiGetNodeQueue() { return CmiNodeQueue; }

void CmiSyncNodeSendAndFree(unsigned int destNode, unsigned int size,
                            void *msg) {

  if (destNode >= Cmi_numnodes || destNode < 0) {
    CmiAbort("Destnode %d out of range %d\n", destNode, Cmi_numnodes);
  }

  if (CmiMyNode() == destNode) {
    CmiNodeQueue->push(msg);
  } else {
    comm_backend::sendAm(destNode, msg, size, comm_backend::MR_NULL,
                         CommLocalHandler, AmHandlerNode);
  }
}

void CmiSyncNodeSend(unsigned int destNode, unsigned int size, void *msg) {
  char *copymsg = (char *)CmiAlloc(size);
  std::memcpy(copymsg, msg, size); // optionally avoid memcpy and block instead
  CmiSyncNodeSendAndFree(destNode, size, copymsg);
}

// fn versions of the above
void CmiSyncSendFn(int destPE, int messageSize, char *msg) {
  CmiSyncSend(destPE, messageSize, (void *)msg);
}

void CmiFreeSendFn(int destPE, int messageSize, char *msg) {
  CmiSyncSendAndFree(destPE, messageSize, (void *)msg);
}

void CmiSyncBroadcastFn(int size, char *msg) {
  CmiSyncBroadcast(size, (void *)msg);
}

void CmiSyncBroadcastAllFn(int size, char *msg) {
  CmiSyncBroadcastAll(size, (void *)msg);
}

void CmiFreeBroadcastFn(int size, char *msg) {
  CmiSyncBroadcastAndFree(size, (void *)msg);
}

void CmiFreeNodeSendFn(int destNode, int size, char *msg) {
  CmiSyncNodeSendAndFree(destNode, size, (void *)msg);
}

void CmiFreeBroadcastAllFn(int size, char *msg) {
  CmiSyncBroadcastAllAndFree(size, (void *)msg);
}

void CmiGroupHandler(void *msg) {
  GroupDef def = (GroupDef)msg;
  GroupDef *table = CmiGroupTable;
  unsigned int hashval, bucket;
  hashval = (def->group.id ^ def->group.pe);
  bucket = hashval % GROUPTAB_SIZE;
  def->next = table[bucket];
  GroupDef newdef =
      (GroupDef)CmiAlloc(sizeof(struct GroupDef_s) + (def->npes * sizeof(int)));
  memcpy(newdef, def, sizeof(struct GroupDef_s) + (def->npes * sizeof(int)));
  table[bucket] = newdef;
}

CmiGroup CmiEstablishGroup(int npes, int *pes) {
  CmiGroup grp;
  GroupDef def;
  int len, i;
  grp.id = CmiGroupCounter++;
  grp.pe = CmiMyPe();
  len = sizeof(struct GroupDef_s) + (npes * sizeof(int));
  def = (GroupDef)CmiAlloc(len);
  def->group = grp;
  def->npes = npes;
  for (i = 0; i < npes; i++)
    def->pes[i] = pes[i];
  CmiSetHandler(def, CmiGroupHandlerIndex);
  CmiGroupHandler(def);
  CmiSyncBroadcastAndFree(len, def);
  return grp;
}

void CmiLookupGroup(CmiGroup grp, int *npes, int **pes) {
  unsigned int hashval, bucket;
  GroupDef def;
  GroupDef *table = CmiGroupTable;
  hashval = (grp.id ^ grp.pe);
  bucket = hashval % GROUPTAB_SIZE;
  for (def = table[bucket]; def; def = def->next) {
    if ((def->group.id == grp.id) && (def->group.pe == grp.pe)) {
      *npes = def->npes;
      *pes = def->pes;
      return;
    }
  }
  *npes = 0;
  *pes = 0;
}

void CmiSyncListSend(int npes, const int *pes, int len, void *msg) {
  for (int i = 0; i < npes; i++) {
    CmiSyncSend(pes[i], len, msg);
  }
}

void CmiSyncListSendFn(int npes, const int *pes, int len, char *msg) {
  CmiSyncListSend(npes, pes, len, (void *)msg);
}

void CmiSyncListSendAndFree(int npes, const int *pes, int len, void *msg) {
  for (int i = 0; i < npes; i++) {
    CmiSyncSendAndFree(pes[i], len, msg);
  }
}

void CmiFreeListSendFn(int npes, const int *pes, int len, char *msg) {
  CmiSyncListSendAndFree(npes, pes, len, (void *)msg);
}

void CmiSyncMulticast(CmiGroup grp, int size, void *msg) {
  int i, *pes;
  int npes;
  CmiLookupGroup(grp, &npes, &pes);
  if (npes == 0) {
    CmiAbort("CmiSyncMulticast: group not found\n");
  }
  CmiSyncListSend(npes, (const int *)pes, size, msg);
}

void CmiSyncMulticastAndFree(CmiGroup grp, int size, void *msg) {
  CmiSyncMulticast(grp, size, msg);
  CmiFree(msg);
}

void CmiSyncMulticastFn(CmiGroup grp, int size, char *msg) {
  CmiSyncMulticast(grp, size, (void *)msg);
}

void CmiFreeMulticastFn(CmiGroup grp, int size, char *msg) {
  CmiSyncMulticastAndFree(grp, size, (void *)msg);
}

void CmiSetHandler(void *msg, int handlerId) {
  CmiMessageHeader *header = (CmiMessageHeader *)msg;
  header->handlerId = handlerId;
}

void CmiSetXHandler(void *msg, int xhandlerId) {
  CmiMessageHeader *header = (CmiMessageHeader *)msg;
  header->swapHandlerId = xhandlerId;
}

int CmiGetHandler(void *msg) {
  CmiMessageHeader *header = (CmiMessageHeader *)msg;
  int handlerId = header->handlerId;
  return handlerId;
}

int CmiGetXHandler(void *msg) {
  CmiMessageHeader *header = (CmiMessageHeader *)msg;
  int xhandlerId = header->swapHandlerId;
  return xhandlerId;
}

CmiHandler CmiGetHandlerFunction(void *msg) {
  int handlerId = CmiGetHandler(msg);
  return CmiGetHandlerTable()->at(handlerId).hdlr;
}

void CmiHandleMessage(void *msg) {
  // process event
  CmiMessageHeader *header = (CmiMessageHeader *)msg;
  int handler = header->handlerId;
  int size = header->messageSize;

  // call handler (takes in pointer to whole message)

  CmiCallHandler(handler, msg);
}
// TODO: implement CmiPrintf
int CmiPrintf(const char *format, ...) {
  va_list args;
  va_start(args, format);

  // Call the actual printf function
  vprintf(format, args);

  va_end(args);
  return 0;
}

double getCurrentTime() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// TODO: implement timer
double CmiWallTimer() { return getCurrentTime() - Cmi_startTime; }

void CmiAbortHelper(const char *source, const char *message,
                    const char *suggestion, int tellDebugger,
                    int framesToSkip) {
  CmiPrintf("------- Processor %d Exiting: %s ------\n"
            "Reason: %s\n",
            CmiMyPe(), source, message);
}

void CmiAbort(const char *format, ...) {
  char newmsg[256];
  va_list args;
  va_start(args, format);
  vsnprintf(newmsg, sizeof(newmsg), format, args);
  va_end(args);
  CmiAbortHelper("Called CmiAbort", newmsg, NULL, 1, 0);

  CmiExitHelper(1);
  abort();
}

int CmiScanf(const char *format, ...) {
  int ret;
  {
    va_list args;
    va_start(args, format);
    {
      ret = vscanf(format, args);
    }
    va_end(args);
  }
  return ret;
}

int CmiError(const char *format, ...) {
  int ret;
  {
    va_list args;
    va_start(args, format);
    {
      ret = vfprintf(stderr, format, args);
      fflush(stderr); /* stderr is always flushed */
    }
    va_end(args);
  }
  return ret;
}

void __CmiEnforceMsgHelper(const char *expr, const char *fileName, int lineNum,
                           const char *msg, ...) {
  CmiAbort("[%d] Assertion \"%s\" failed in file %s line %d.\n", CmiMyPe(),
           expr, fileName, lineNum);
}

// TODO: implememt
void CmiInitCPUTopology(char **argv) {}

// TODO: implememt
void CmiInitCPUAffinity(char **argv) {}

bool CmiGetIdle() { return idle_condition; }

void CmiSetIdle(bool idle) { idle_condition = idle; }

double CmiGetIdleTime() { return idle_time; }

void CmiSetIdleTime(double time) { idle_time = time; }

/*****************************************************************************
 *
 * Command-Line Argument (CLA) parsing routines.
 *
 *****************************************************************************/

static int usageChecked =
    0; /* set when argv has been searched for a usage request */
static int printUsage = 0; /* if set, print command-line usage information */
static const char *CLAformatString = "%20s %10s %s\n";

/** This little list of CLA's holds the argument descriptions until it's
   safe to print them--it's needed because the netlrts- versions don't have
   printf until they're pretty well started.
 */
typedef struct {
  const char *arg;   /* Flag name, like "-foo"*/
  const char *param; /* Argument's parameter type, like "integer" or "none"*/
  const char *desc;  /* Human-readable description of what it does */
} CLA;
static int CLAlistLen = 0;
static int CLAlistMax = 0;
static CLA *CLAlist = NULL;

/** Add this CLA */
static void CmiAddCLA(const char *arg, const char *param, const char *desc) {
  int i;
  if (CmiMyPe() != 0)
    return; /*Don't bother if we're not PE 0*/
  if (desc == NULL)
    return;           /*It's an internal argument*/
  if (usageChecked) { /* Printf should work now */
    if (printUsage)
      CmiPrintf(CLAformatString, arg, param, desc);
  } else { /* Printf doesn't work yet-- just add to the list.
        This assumes the const char *'s are static references,
        which is probably reasonable. */
    CLA *temp;
    i = CLAlistLen++;
    if (CLAlistLen > CLAlistMax) { /*Grow the CLA list */
      CLAlistMax = 16 + 2 * CLAlistLen;
      temp = (CLA *)realloc(CLAlist, sizeof(CLA) * CLAlistMax);
      if (temp != NULL) {
        CLAlist = temp;
      } else {
        free(CLAlist);
        CmiAbort("Reallocation failed for CLAlist\n");
      }
    }
    CLAlist[i].arg = arg;
    CLAlist[i].param = param;
    CLAlist[i].desc = desc;
  }
}

/** Print out the stored list of CLA's */
static void CmiPrintCLAs(void) {
  int i;
  if (CmiMyPe() != 0)
    return; /*Don't bother if we're not PE 0*/
  CmiPrintf("Converse Machine Command-line Parameters:\n ");
  CmiPrintf(CLAformatString, "Option:", "Parameter:", "Description:");
  for (i = 0; i < CLAlistLen; i++) {
    CLA *c = &CLAlist[i];
    CmiPrintf(CLAformatString, c->arg, c->param, c->desc);
  }
}

/**
 * Determines if command-line usage information should be printed--
 * that is, if a "-?", "-h", or "--help" flag is present.
 * Must be called after printf is setup.
 */
void CmiArgInit(char **argv) {
  int i;
  // CmiLock(_smp_mutex);
  for (i = 0; argv[i] != NULL; i++) {
    if (0 == strcmp(argv[i], "-?") || 0 == strcmp(argv[i], "-h") ||
        0 == strcmp(argv[i], "--help")) {
      printUsage = 1;
      /* Don't delete arg:  CmiDeleteArgs(&argv[i],1);
        Leave it there for user program to see... */
      CmiPrintCLAs();
    }
  }
  if (CmiMyPe() == 0) { /* Throw away list of stored CLA's */
    CLAlistLen = CLAlistMax = 0;
    free(CLAlist);
    CLAlist = NULL;
  }
  usageChecked = 1;
  // CmiUnlock(_smp_mutex);
}

/** Return 1 if we're currently printing command-line usage information. */
int CmiArgGivingUsage(void) { return (CmiMyPe() == 0) && printUsage; }

/** Identifies the module that accepts the following command-line parameters */
void CmiArgGroup(const char *parentName, const char *groupName) {
  if (CmiArgGivingUsage()) {
    if (groupName == NULL)
      groupName = parentName; /* Start of a new group */
    CmiPrintf("\n%s Command-line Parameters:\n", groupName);
  }
}

/** Count the number of non-NULL arguments in list*/
int CmiGetArgc(char **argv) {
  int i = 0, argc = 0;
  if (argv)
    while (argv[i++] != NULL)
      argc++;
  return argc;
}

/** Return a new, heap-allocated copy of the argv array*/
char **CmiCopyArgs(char **argv) {
  int argc = CmiGetArgc(argv);
  char **ret = (char **)malloc(sizeof(char *) * (argc + 1));
  int i;
  for (i = 0; i <= argc; i++)
    ret[i] = argv[i];
  return ret;
}

/** Delete the first k argument from the given list, shifting
all other arguments down by k spaces.
e.g., argv=={"a","b","c","d",NULL}, k==3 modifies
argv={"d",NULL,"c","d",NULL}
*/
void CmiDeleteArgs(char **argv, int k) {
  int i = 0;
  while ((argv[i] = argv[i + k]) != NULL)
    i++;
}

/** Find the given argment and string option in argv.
If the argument is present, set the string option and
delete both from argv.  If not present, return NULL.
e.g., arg=="-name" returns "bob" from
argv=={"a.out","foo","-name","bob","bar"},
and sets argv={"a.out","foo","bar"};
*/
int CmiGetArgStringDesc(char **argv, const char *arg, char **optDest,
                        const char *desc) {
  int i;
  CmiAddCLA(arg, "string", desc);
  for (i = 0; argv[i] != NULL; i++)
    if (0 == strcmp(argv[i], arg)) { /*We found the argument*/
      if (argv[i + 1] == NULL)
        CmiAbort("Argument not complete!");
      *optDest = argv[i + 1];
      CmiDeleteArgs(&argv[i], 2);
      return 1;
    }
  return 0; /*Didn't find the argument*/
}
int CmiGetArgString(char **argv, const char *arg, char **optDest) {
  return CmiGetArgStringDesc(argv, arg, optDest, "");
}

/** Find the given argument and floating-point option in argv.
Remove it and return 1; or return 0.
*/
int CmiGetArgDoubleDesc(char **argv, const char *arg, double *optDest,
                        const char *desc) {
  char *number = NULL;
  CmiAddCLA(arg, "number", desc);
  if (!CmiGetArgStringDesc(argv, arg, &number, NULL))
    return 0;
  if (1 != sscanf(number, "%lg", optDest))
    return 0;
  return 1;
}
int CmiGetArgDouble(char **argv, const char *arg, double *optDest) {
  return CmiGetArgDoubleDesc(argv, arg, optDest, "");
}

/** Find the given argument and integer option in argv.
If the argument is present, parse and set the numeric option,
delete both from argv, and return 1. If not present, return 0.
e.g., arg=="-pack" matches argv=={...,"-pack","27",...},
argv=={...,"-pack0xf8",...}, and argv=={...,"-pack=0777",...};
but not argv=={...,"-packsize",...}.
*/
int CmiGetArgIntDesc(char **argv, const char *arg, int *optDest,
                     const char *desc) {
  int i;
  int argLen = strlen(arg);
  CmiAddCLA(arg, "integer", desc);
  for (i = 0; argv[i] != NULL; i++)
    if (0 ==
        strncmp(argv[i], arg, argLen)) { /*We *may* have found the argument*/
      const char *opt = NULL;
      int nDel = 0;
      switch (argv[i][argLen]) {
      case 0: /* like "-p","27" */
        opt = argv[i + 1];
        nDel = 2;
        break;
      case '=': /* like "-p=27" */
        opt = &argv[i][argLen + 1];
        nDel = 1;
        break;
      case '-':
      case '+':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        /* like "-p27" */
        opt = &argv[i][argLen];
        nDel = 1;
        break;
      default:
        continue; /*False alarm-- skip it*/
      }
      if (opt == NULL) {
        fprintf(stderr,
                "Command-line flag '%s' expects a numerical argument, "
                "but none was provided\n",
                arg);
        CmiAbort("Bad command-line argument\n");
      }
      if (sscanf(opt, "%i", optDest) < 1) {
        /*Bad command line argument-- die*/
        fprintf(stderr,
                "Cannot parse %s option '%s' "
                "as an integer.\n",
                arg, opt);
        CmiAbort("Bad command-line argument\n");
      }
      CmiDeleteArgs(&argv[i], nDel);
      return 1;
    }
  return 0; /*Didn't find the argument-- dest is unchanged*/
}
int CmiGetArgInt(char **argv, const char *arg, int *optDest) {
  return CmiGetArgIntDesc(argv, arg, optDest, "");
}

int CmiGetArgLongDesc(char **argv, const char *arg, CmiInt8 *optDest,
                      const char *desc) {
  int i;
  int argLen = strlen(arg);
  CmiAddCLA(arg, "integer", desc);
  for (i = 0; argv[i] != NULL; i++)
    if (0 ==
        strncmp(argv[i], arg, argLen)) { /*We *may* have found the argument*/
      const char *opt = NULL;
      int nDel = 0;
      switch (argv[i][argLen]) {
      case 0: /* like "-p","27" */
        opt = argv[i + 1];
        nDel = 2;
        break;
      case '=': /* like "-p=27" */
        opt = &argv[i][argLen + 1];
        nDel = 1;
        break;
      case '-':
      case '+':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        /* like "-p27" */
        opt = &argv[i][argLen];
        nDel = 1;
        break;
      default:
        continue; /*False alarm-- skip it*/
      }
      if (opt == NULL) {
        fprintf(stderr,
                "Command-line flag '%s' expects a numerical argument, "
                "but none was provided\n",
                arg);
        CmiAbort("Bad command-line argument\n");
      }
      if (sscanf(opt, "%" SCNd64, optDest) < 1) {
        /*Bad command line argument-- die*/
        fprintf(stderr,
                "Cannot parse %s option '%s' "
                "as a long integer.\n",
                arg, opt);
        CmiAbort("Bad command-line argument\n");
      }
      CmiDeleteArgs(&argv[i], nDel);
      return 1;
    }
  return 0; /*Didn't find the argument-- dest is unchanged*/
}
int CmiGetArgLong(char **argv, const char *arg, CmiInt8 *optDest) {
  return CmiGetArgLongDesc(argv, arg, optDest, "");
}

/** Find the given argument in argv.  If present, delete
it and return 1; if not present, return 0.
e.g., arg=="-foo" matches argv=={...,"-foo",...} but not
argv={...,"-foobar",...}.
*/
int CmiGetArgFlagDesc(char **argv, const char *arg, const char *desc) {
  int i;
  CmiAddCLA(arg, "", desc);
  for (i = 0; argv[i] != NULL; i++)
    if (0 == strcmp(argv[i], arg)) { /*We found the argument*/
      CmiDeleteArgs(&argv[i], 1);
      return 1;
    }
  return 0; /*Didn't find the argument*/
}
int CmiGetArgFlag(char **argv, const char *arg) {
  return CmiGetArgFlagDesc(argv, arg, "");
}

void CmiDeprecateArgInt(char **argv, const char *arg, const char *desc,
                        const char *warning) {
  int dummy = 0, found = CmiGetArgIntDesc(argv, arg, &dummy, desc);

  if (found)
    CmiPrintf("%s\n", warning);
}

CmiNodeLock CmiCreateLock() {
  CmiNodeLock lock = (CmiNodeLock)malloc(sizeof(pthread_mutex_t));
  pthread_mutex_init(lock, NULL);
  return lock;
}

void CmiDestroyLock(CmiNodeLock lock) {
  pthread_mutex_destroy(lock);
  free(lock);
}

void CmiLock(CmiNodeLock lock) { pthread_mutex_lock(lock); }

void CmiUnlock(CmiNodeLock lock) { pthread_mutex_unlock(lock); }

int CmiTryLock(CmiNodeLock lock) { return pthread_mutex_trylock(lock); }
