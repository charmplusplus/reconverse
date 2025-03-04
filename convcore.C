//+pe <N> threads, each running a scheduler
#include "convcore.h"
#include "scheduler.h"
#include "barrier.h"
#include "queue.h"
#include "converse.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <cstring>
#include <cstdarg>

// GLOBALS
int Cmi_argc;
static char **Cmi_argv;
int Cmi_npes;
int Cmi_nranks;                                // TODO: this isnt used in old converse, but we need to know how many PEs are on our node?
std::vector<CmiHandlerInfo> **CmiHandlerTable; // array of handler vectors
ConverseQueue<void *> *CmiNodeQueue;
double Cmi_startTime;

// PE LOCALS that need global access sometimes
static ConverseQueue<void *> **Cmi_queues; // array of queue pointers

// PE LOCALS
thread_local int Cmi_myrank;
thread_local CmiState *Cmi_state;

// TODO: padding for all these thread_locals and cmistates?

void CmiCallHandler(int handler, void *msg)
{
    CmiGetHandlerTable()->at(handler).hdlr(msg);
}

void *converseRunPe(void *args)
{
    // init state
    int pe = *(int *)args;
    CmiInitState(pe);

    // barrier to ensure all global structs are initialized
    CmiNodeBarrier();

    CthInit((char**) &args);
    CthSchedInit();

    // call initial function and start scheduler
    Cmi_startfn(Cmi_argc, Cmi_argv);
    CsdScheduler();

    return NULL;
}

void CmiStartThreads()
{
    pthread_t threadId[Cmi_npes];

    // TODO: how to get enumerated pe nums for each thread?
    // this would be much cleaner with std::threads
    int threadPeNums[Cmi_npes];

    // allocate global arrayss
    Cmi_queues = new ConverseQueue<void *> *[Cmi_npes];
    CmiHandlerTable = new std::vector<CmiHandlerInfo> *[Cmi_npes];

    for (int i = 0; i < Cmi_npes; i++)
    {
        threadPeNums[i] = i;
        pthread_create(&threadId[i], NULL, converseRunPe, &threadPeNums[i]);
    }
    for (int i = 0; i < Cmi_npes; i++)
    {
        pthread_join(threadId[i], NULL);
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

    // NOTE: calling CmiNumPes() here it sometimes returns zero
    printf("Charm++> Running in SMP mode: %d processes\n", Cmi_npes);

    Cmi_argc = argc - 2; // TODO: Cmi_argc doesn't include runtime args?
    Cmi_argv = (char **)malloc(sizeof(char *) * (argc + 1));
    int i;
    for (i = 2; i <= argc; i++)
        Cmi_argv[i - 2] = argv[i];
    Cmi_startfn = fn;

    CmiStartThreads();
    free(Cmi_argv);
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
    Cmi_state->pe = rank;
    Cmi_state->rank = rank; // TODO: for now, pe is just thread index
    Cmi_state->node = 0;    // TODO: get node
    Cmi_state->stopFlag = 0;

    Cmi_myrank = rank;

    // allocate global entries
    ConverseQueue<void *> *queue = new ConverseQueue<void *>();
    std::vector<CmiHandlerInfo> *handlerTable = new std::vector<CmiHandlerInfo>();

    CmiNodeQueue = new ConverseQueue<void *>();

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
    return CmiMyRank(); // TODO: fix once in multi node context
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
    return Cmi_npes; // TODO: get node size (this is not the same)
}

int CmiNumPes()
{
    return Cmi_npes;
}

std::vector<CmiHandlerInfo> *CmiGetHandlerTable()
{
    return CmiHandlerTable[CmiMyRank()];
}

void CmiPushPE(int destPE, int messageSize, void *msg)
{
    Cmi_queues[destPE]->push(msg);
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
    int destNode = 0; // TODO: get node from destPE?
    if (CmiMyNode() == destNode)
    {
        CmiPushPE(destPE, messageSize, msg);
    }
    else
    {
        // TODO: handle off node message send
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

ConverseQueue<void *> *CmiGetNodeQueue()
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
        // TODO: if off node
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
