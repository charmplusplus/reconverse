//+pe <N> threads, each running a scheduler
#include "barrier.h"
#include "converse_internal.h"
#include "queue.h"
#include "scheduler.h"

#include <cinttypes>
#include <conv-rdma.h>
#include <cstdarg>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>

// GLOBALS
int Cmi_argc;
static char **Cmi_argv;
char **Cmi_argvcopy;
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
int CharmLibInterOperate;
int _immediateLock = 0;
std::atomic<int> _cleanUp = 0;
void *memory_stack_top;
CmiNodeLock _smp_mutex;
CpvDeclare(std::vector<NcpyOperationInfo *>, newZCPupGets);
CpvCExtern(int, interopExitFlag);
CpvDeclare(int, interopExitFlag);
std::atomic<int> ckExitComplete{0};
int quietMode;
int quietModeRequested;
int userDrivenMode;
int _replaySystem = 0;

void CldModuleInit(char **);

// PE LOCALS that need global access sometimes
static ConverseQueue<void *> **Cmi_queues; // array of queue pointers

// PE LOCALS
thread_local int Cmi_myrank;
thread_local CmiState *Cmi_state;
thread_local bool idle_condition;
thread_local double idle_time;

// Special operation handlers (TODO: should these be special values instead like
// the exit handler)
int Cmi_exitHandler;

// TODO: padding for all these thread_locals and cmistates?

comm_backend::AmHandler AmHandlerPE;
comm_backend::AmHandler AmHandlerNode;

void CommLocalHandler(comm_backend::Status status) {
  CmiFree(const_cast<void *>(status.local_buf));
}

void CommRemoteHandlerPE(comm_backend::Status status) {
  CmiMessageHeader *header = (CmiMessageHeader *)status.local_buf;
  int destPE = header->destPE;
  CmiPushPE(destPE, status.size, const_cast<void *>(status.local_buf));
}

void CommRemoteHandlerNode(comm_backend::Status status) {
  CmiNodeQueue->push(const_cast<void *>(status.local_buf));
}

void CmiCallHandler(int handler, void *msg) {
  CmiHandlerInfo h = CmiGetHandlerTable()->at(handler);
  if (h.userPtr) {
    h.exhdlr(msg, h.userPtr);
  } else {
    h.hdlr(msg);
  }
}

void converseRunPe(int rank) {
  // init state
  CmiInitState(rank);

  // init things like cld module, ccs, etc
  CldModuleInit(Cmi_argv);
#ifdef SET_CPU_AFFINITY
  CmiSetCPUAffinity(rank);
#endif

  Cmi_exitHandler = CmiRegisterHandler(CmiExitHandler);
  collectiveInit();
  // Cmi_multicastHandler = CmiRegisterHandler(CmiMulticastHandler);

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

  _smp_mutex = CmiCreateLock();

  // make sure the queues are allocated before PEs start sending messages around
  comm_backend::barrier();

  std::vector<std::thread> threads;
  for (int i = 0; i < Cmi_mynodesize; i++) {
    std::thread t(converseRunPe, i);
    threads.push_back(std::move(t));
  }

  for (auto &thread : threads) {
    thread.join();
  }

  // make sure all PEs are done before we free the queues.
  comm_backend::barrier();
  comm_backend::exit();
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

  Cmi_argvcopy = CmiCopyArgs(argv);
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
  CharmLibInterOperate = 0;

  CmiStartThreads();
  free(Cmi_argv);
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
  CpvInitialize(std::vector<NcpyOperationInfo *>,
                newZCPupGets); // Check if this is necessary
  CpvInitialize(int, interopExitFlag);
  CpvAccess(interopExitFlag) = 0;
  CmiOnesidedDirectInit();
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

int CmiPhysicalNodeID(int pe) { return CmiNodeOf(pe); }

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

void CmiNumberHandler(int n, CmiHandler h) {
  CmiHandlerInfo newEntry;
  newEntry.hdlr = h;
  newEntry.userPtr = nullptr;
  if (n >= CmiGetHandlerTable()->size()) {
    CmiGetHandlerTable()->resize(n + 1);
  }
  CmiGetHandlerTable()->at(n) = newEntry;
}

void CmiNumberHandlerEx(int n, CmiHandlerEx h, void *userPtr) {
  CmiHandlerInfo newEntry;
  newEntry.exhdlr = h;
  newEntry.userPtr = userPtr;
  if (n >= CmiGetHandlerTable()->size()) {
    CmiGetHandlerTable()->resize(n + 1);
  }
  CmiGetHandlerTable()->at(n) = newEntry;
}

void CmiPushPE(int destPE, int messageSize, void *msg) {
  int rank = CmiRankOf(destPE);
  CmiAssertMsg(
      rank >= 0 && rank < Cmi_mynodesize,
      "CmiPushPE(myPe: %d, destPe: %d, nodeSize: %d): rank out of range",
      CmiMyPe(), destPE, Cmi_mynodesize);
  Cmi_queues[rank]->push(msg);
}

void CmiPushPE(int destPE, void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  int messageSize = header->messageSize;
  CmiPushPE(destPE, messageSize, msg);
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

// header ref count methods

static void *CmiAllocFindEnclosing(void *blk) {
  int refCount = REFFIELD(blk);
  while (refCount < 0) {
    blk = (void *)((char *)blk + refCount); /* Jump to enclosing block */
    refCount = REFFIELD(blk);
  }
  return blk;
}

int CmiGetReference(void *blk) { return REFFIELD(CmiAllocFindEnclosing(blk)); }

void CmiReference(void *blk) { REFFIELDINC(CmiAllocFindEnclosing(blk)); }

int CmiSize(void *blk) { return SIZEFIELD(blk); }

void CmiMemoryMarkBlock(void *blk) {}

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
    comm_backend::issueAm(destNode, msg, messageSize, comm_backend::MR_NULL,
                          CommLocalHandler, AmHandlerPE,
                          nullptr); // Commlocalhandler will free msg
  }
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

// HANDLER TOOLS
int CmiRegisterHandler(CmiHandler h) {
  // add handler to vector
  std::vector<CmiHandlerInfo> *handlerVector = CmiGetHandlerTable();

  handlerVector->push_back({h, nullptr});
  return handlerVector->size() - 1;
}

int CmiRegisterHandlerEx(CmiHandlerEx h, void *userPtr) {
  // add handler to vector
  std::vector<CmiHandlerInfo> *handlerVector = CmiGetHandlerTable();

  CmiHandlerInfo newEntry;
  newEntry.exhdlr = h;
  newEntry.userPtr = userPtr;
  handlerVector->push_back(newEntry);
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

void CmiAssignOnce(int *variable, int value) {
  if (CmiMyRank() == 0) {
    *variable = value;
  }
  CmiNodeAllBarrier();
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
    comm_backend::issueAm(destNode, msg, size, comm_backend::MR_NULL,
                          CommLocalHandler, AmHandlerNode, nullptr);
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

double CmiStartTimer() { return 0.0; }

double CmiInitTime(void) { return Cmi_startTime; }

int CmiTimerAbsolute() {
  // Reconverse FIXME: The purpose of this function is unclear
  return 0;
}

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

// empty function to satisfy charm
void CommunicationServerThread(int sleepTime) {}

// start and stop interop schedulers

void StartInteropScheduler() {
  CsdScheduler(-1);
  CmiNodeAllBarrier();
}

void StopInteropScheduler() { CpvAccess(interopExitFlag) = 1; }

static char *CopyMsg(char *msg, int len) {
  char *copy = (char *)CmiAlloc(len);
#if CMK_ERROR_CHECKING
  if (!copy) {
    CmiAbort("Error: out of memory in machine layer\n");
  }
#endif
  memcpy(copy, msg, len);
  return copy;
}

void CmiForwardMsgToPeers(int size, char *msg) {
  /* FIXME: now it's just a flat p2p send!! When node size is large,
   * it should also be sent in a tree
   */

  int exceptRank = CmiMyRank();
  if (CMI_MSG_NOKEEP(msg)) {
    for (int i = 0; i < exceptRank; i++) {
      CmiReference(msg);
      CmiPushPE(i, msg);
    }
    for (int i = exceptRank + 1; i < CmiMyNodeSize(); i++) {
      CmiReference(msg);
      CmiPushPE(i, msg);
    }
  } else {
    for (int i = 0; i < exceptRank; i++) {
      CmiPushPE(i, CopyMsg(msg, size));
    }
    for (int i = exceptRank + 1; i < CmiMyNodeSize(); i++) {
      CmiPushPE(i, CopyMsg(msg, size));
    }
  }
}

// Since we are not implementing converse level seed balancers yet
void LBTopoInit() {}
