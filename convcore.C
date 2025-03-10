//+pe <N> threads, each running a scheduler
#include "convcore.h"
#include "scheduler.h"
#include "barrier.h"
#include "queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <cstring>
#include <cstdarg>
#include <thread>

// GLOBALS
int Cmi_argc;
static char **Cmi_argv;
int Cmi_npes;
int Cmi_nranks;                                // TODO: this isnt used in old converse, but we need to know how many PEs are on our node?
int Cmi_mynode;
int Cmi_mynodesize;
int Cmi_numnodes;
int Cmi_nodestart;
std::vector<CmiHandlerInfo> **CmiHandlerTable; // array of handler vectors
ConverseNodeQueue<void *> *CmiNodeQueue;
double Cmi_startTime;


// PE LOCALS that need global access sometimes
static ConverseQueue<void *> **Cmi_queues; // array of queue pointers

// PE LOCALS
thread_local int Cmi_myrank;
thread_local CmiState *Cmi_state;
thread_local bool idle_condition;
thread_local double idle_time;

// TODO: padding for all these thread_locals and cmistates?

comm_backend::AmHandler AmHandlerPE;
comm_backend::AmHandler AmHandlerNode;

void CommLocalHandler(comm_backend::Status status)
{
    CmiFree(status.msg);
}

void CommRemoteHandlerPE(comm_backend::Status status) {
    CmiMessageHeader *header = (CmiMessageHeader *)status.msg;
    int destPE = header->destPE;
    CmiPushPE(destPE, status.size, status.msg);
}

void CommRemoteHandlerNode(comm_backend::Status status) {
    CmiNodeQueue->push(status.msg);
}

void CmiCallHandler(int handler, void *msg)
{
    CmiGetHandlerTable()->at(handler).hdlr(msg);
}

void converseRunPe(int rank)
{
    // init state
    CmiInitState(rank);

    // barrier to ensure all global structs are initialized
    CmiNodeBarrier();

    // call initial function and start scheduler
    Cmi_startfn(Cmi_argc, Cmi_argv);
    CsdScheduler();
}

void CmiStartThreads()
{
    // allocate global arrayss
    Cmi_queues = new ConverseQueue<void *> *[Cmi_mynodesize];
    CmiHandlerTable = new std::vector<CmiHandlerInfo> *[Cmi_mynodesize];
    CmiNodeQueue = new ConverseNodeQueue<void *>();

    std::vector<std::thread> threads;
    for (int i = 0; i < Cmi_mynodesize; i++)
    {
        std::thread t(converseRunPe, i);
        threads.push_back(std::move(t));
    }

    for (auto &thread : threads)
    {
        thread.join();
    }
}

// argument form: ./prog +pe <N>
// TODO: this function need error checking
// TODO: the input parsing, cmi_arg parsing is not done/robust
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched, int initret)
{

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
    if (Cmi_npes < Cmi_numnodes)
    {
        fprintf(stderr, "Error: Number of PEs must be greater than or equal to number of nodes\n");
        exit(1);
    }
    if (Cmi_npes % Cmi_numnodes != 0)
    {
        fprintf(stderr, "Error: Number of PEs must be a multiple of number of nodes\n");
        exit(1);
    }
    Cmi_mynodesize = Cmi_npes / Cmi_numnodes;
    Cmi_nodestart = Cmi_mynode * Cmi_mynodesize;
    // register am handlers
    AmHandlerPE = comm_backend::registerAmHandler(CommRemoteHandlerPE);
    AmHandlerNode = comm_backend::registerAmHandler(CommRemoteHandlerNode);

    Cmi_startfn = fn;

    CmiStartThreads();
    free(Cmi_argv);
    
    comm_backend::exit();
}

// CMI STATE
CmiState *
CmiGetState(void)
{
    return Cmi_state;
};

void CmiInitState(int rank)
{
    // allocate state
    Cmi_state = new CmiState;
    Cmi_state->pe = Cmi_nodestart + rank;
    Cmi_state->rank = rank;
    Cmi_state->node = Cmi_mynode;
    Cmi_state->stopFlag = 0;

    Cmi_myrank = rank;
    CmiSetIdle(false);
    CmiSetIdleTime(0.0);

    // allocate global entries
    ConverseQueue<void *> *queue = new ConverseQueue<void *>();
    std::vector<CmiHandlerInfo> *handlerTable = new std::vector<CmiHandlerInfo>();

    Cmi_queues[Cmi_myrank] = queue;
    CmiHandlerTable[Cmi_myrank] = handlerTable;

    CcdModuleInit();
}

ConverseQueue<void *> *CmiGetQueue(int rank)
{
    return Cmi_queues[rank];
}

int CmiMyRank()
{
    return CmiGetState()->rank;
}

int CmiMyPe()
{
    return CmiGetState()->pe;
}

int CmiStopFlag()
{
    return CmiGetState()->stopFlag;
}

int CmiMyNode()
{
    return CmiGetState()->node;
}

int CmiMyNodeSize()
{
    return Cmi_mynodesize;
}

int CmiNumNodes()
{
    return Cmi_numnodes;
}

int CmiNumPes()
{
    return Cmi_npes;
}

int CmiNodeOf(int pe)
{
    return pe / Cmi_mynodesize;
}

int CmiRankOf(int pe)
{
    return pe % Cmi_mynodesize;
}

int CmiNodeFirst(int node)
{
    return node * Cmi_mynodesize;
}

std::vector<CmiHandlerInfo> *CmiGetHandlerTable()
{
    return CmiHandlerTable[CmiMyRank()];
}

void CmiPushPE(int destPE, int messageSize, void *msg)
{
    int rank = CmiRankOf(destPE);
    Cmi_queues[rank]->push(msg);
}

void *CmiAlloc(int size)
{
    return malloc(size);
}

void CmiFree(void *msg)
{
    free(msg);
}

void CmiSyncSend(int destPE, int messageSize, void *msg)
{
    char *copymsg = (char *)CmiAlloc(messageSize);
    std::memcpy(copymsg, msg, messageSize);
    CmiSyncSendAndFree(destPE, messageSize, copymsg);
}

void CmiSyncSendAndFree(int destPE, int messageSize, void *msg)
{
    // printf("Sending message to PE %d\n", destPE);
    CmiMessageHeader *header = static_cast<CmiMessageHeader*>(msg);
    header->destPE = destPE;
    header->messageSize = messageSize;
    int destNode = CmiNodeOf(destPE);
    if (CmiMyNode() == destNode)
    {
        CmiPushPE(destPE, messageSize, msg);
    }
    else
    {
        comm_backend::sendAm(destNode, msg, messageSize, CommLocalHandler, AmHandlerPE);
    }
}

void CmiSyncBroadcast(int size, void *msg)
{
    CmiState *cs = CmiGetState();

    for (int i = cs->pe + 1; i < Cmi_npes; i++)
        CmiSyncSend(i, size, msg);

    for (int i = 0; i < cs->pe; i++)
        CmiSyncSend(i, size, msg);
}

void CmiSyncBroadcastAndFree(int size, void *msg)
{
    CmiSyncBroadcast(size, msg);
    CmiFree(msg);
}

void CmiSyncBroadcastAll(int size, void *msg)
{
    for (int i = 0; i < Cmi_npes; i++)
        CmiSyncSend(i, size, msg);
}

void CmiSyncBroadcastAllAndFree(int size, void *msg)
{
    CmiState *cs = CmiGetState();

    for (int i = cs->pe + 1; i < Cmi_npes; i++)
        CmiSyncSend(i, size, msg);

    for (int i = 0; i < cs->pe; i++)
        CmiSyncSend(i, size, msg);

    CmiSyncSendAndFree(cs->pe, size, msg);
}

// HANDLER TOOLS
int CmiRegisterHandler(CmiHandler h)
{
    // add handler to vector
    std::vector<CmiHandlerInfo> *handlerVector = CmiGetHandlerTable();

    handlerVector->push_back({h, nullptr});
    return handlerVector->size() - 1;
}

void CmiNodeBarrier(void)
{
    static Barrier nodeBarrier(CmiMyNodeSize());
    nodeBarrier.wait(); // TODO: this may be broken...
}

// TODO: in the original converse, this variant blocks comm thread as well. CmiNodeBarrier does not.
void CmiNodeAllBarrier()
{
    static Barrier nodeBarrier(CmiMyNodeSize());
    nodeBarrier.wait();
}

void CsdExitScheduler()
{
    CmiGetState()->stopFlag = 1;
}

ConverseNodeQueue<void *> *CmiGetNodeQueue()
{
    return CmiNodeQueue;
}

void CmiSyncNodeSendAndFree(unsigned int destNode, unsigned int size, void *msg)
{
    if (CmiMyNode() == destNode)
    {
        CmiNodeQueue->push(msg);
    }
    else
    {
        comm_backend::sendAm(destNode, msg, size, CommLocalHandler, AmHandlerNode);
    }
}

void CmiSetHandler(void *msg, int handlerId)
{
    CmiMessageHeader *header = (CmiMessageHeader *)msg;
    header->handlerId = handlerId;
}

// TODO: implement CmiPrintf
int CmiPrintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);

    // Call the actual printf function
    vprintf(format, args);

    va_end(args);
    return 0;
}

double getCurrentTime()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// TODO: implement timer
double CmiWallTimer()
{
    return getCurrentTime() - Cmi_startTime;
}

int CmiGetArgc(char **argv)
{
    // TODO: is this supposed to be argc after runtime params are extracted?
    return Cmi_argc;
}

// TODO: implement
void CmiAbort(const char *format, ...)
{
    printf("CMI ABORT\n");
    abort();
}

// TODO: implememt
void CmiInitCPUTopology(char **argv)
{
}

// TODO: implememt
void CmiInitCPUAffinity(char **argv)
{
}

bool CmiGetIdle()
{
    return idle_condition;
}

void CmiSetIdle(bool idle)
{
    idle_condition = idle;
}


double CmiGetIdleTime()
{
    return idle_time;
}

void CmiSetIdleTime(double time)
{
    idle_time = time;
}