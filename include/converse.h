// function declarations that a user program can call

#ifndef CONVERSE_H
#define CONVERSE_H

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <pthread.h>

using CmiInt1 = std::int8_t;
using CmiInt2 = std::int16_t;
using CmiInt4 = std::int32_t;
using CmiInt8 = std::int64_t;
using CmiUint1 = std::uint8_t;
using CmiUint2 = std::uint16_t;
using CmiUint4 = std::uint32_t;
using CmiUint8 = std::uint64_t;

#include <stdint.h>
typedef int8_t CMK_TYPEDEF_INT1;
typedef int16_t CMK_TYPEDEF_INT2;
typedef int32_t CMK_TYPEDEF_INT4;
typedef int64_t CMK_TYPEDEF_INT8;
typedef uint8_t CMK_TYPEDEF_UINT1;
typedef uint16_t CMK_TYPEDEF_UINT2;
typedef uint32_t CMK_TYPEDEF_UINT4;
typedef uint64_t CMK_TYPEDEF_UINT8;
typedef intptr_t CmiIntPtr;

typedef CMK_TYPEDEF_INT1 CmiInt1;
typedef CMK_TYPEDEF_INT2 CmiInt2;
typedef CMK_TYPEDEF_INT4 CmiInt4;
typedef CMK_TYPEDEF_INT8 CmiInt8;
typedef CMK_TYPEDEF_UINT1 CmiUInt1;
typedef CMK_TYPEDEF_UINT2 CmiUInt2;
typedef CMK_TYPEDEF_UINT4 CmiUInt4;
typedef CMK_TYPEDEF_UINT8 CmiUInt8;
typedef __int128_t CmiInt16;
typedef __uint128_t CmiUInt16;

// NOTE: these are solely for backwards compatibility
// Do not use in reconverse impl

#define CMK_TAG(x, y) x##y##_
#define CMK_CONCAT(x, y) x##y

#define CpvDeclare(t, v) t *CMK_TAG(Cpv_, v)
#define CpvStaticDeclare(t, v) static t *CMK_TAG(Cpv_, v)

#define CpvInitialize(t, v)                                                    \
  do {                                                                         \
    if (CmiMyRank()) {                                                         \
      CmiNodeBarrier();                                                        \
    } else {                                                                   \
      CMK_TAG(Cpv_, v) = new t[CmiMyNodeSize()];                               \
      CmiNodeBarrier();                                                        \
    }                                                                          \
  } while (0)
;

#define CpvAccess(v) CMK_TAG(Cpv_, v)[CmiMyRank()]
#define CpvAccessOther(v, r) CMK_TAG(Cpv_, v)[r]
#define CpvExtern(t, v) extern t *CMK_TAG(Cpv_, v)
#define CpvInitialized(v) (0 != CMK_TAG(Cpv_, v))

#define CpvCExtern(t, v) extern "C" t *CMK_TAG(Cpv_, v)

#define CsvDeclare(t, v) t v
#define CsvStaticDeclare(t, v) static t v
#define CsvExtern(t, v) extern t v
#define CsvInitialize(t, v)                                                    \
  do {                                                                         \
  } while (0)
#define CsvInitialized(v) 1
#define CsvAccess(v) v

#define MESSAGE_PHASE_CHECK(msg)

// alignment
#define CMIALIGN(x, n) (size_t)((~((size_t)n - 1)) & ((x) + (n - 1)))
#define ALIGN8(x) CMIALIGN(x, 8)
#define ALIGN16(x) CMIALIGN(x, 16)
#define ALIGN_BYTES 16U // should this be 18U or 8U?
#define ALIGN_DEFAULT(x) CMIALIGN(x, ALIGN_BYTES)
#define CMIPADDING(x, n) (CMIALIGN((x), (n)) - (size_t)(x))

// End of NOTE

typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr); // ignore for now

typedef void (*CldPackFn)(void *msg);

typedef void (*CldInfoFn)(void *msg, CldPackFn *packer, int *len, int *queueing,
                          int *priobits, unsigned int **prioptr);

typedef int (*CldEstimator)(void);

#define CmiMessageDestPENode static_cast<CmiUint4>(-1)
typedef struct Header {
  CmiInt2 handlerId;
  CmiUint4 destPE; // global ID of destination PE
  int messageSize;
  // used for bcast (bcast source pe/node), multicast (group id), reductions
  // (reduction id)
  CmiUint4 collectiveMetaInfo;
  // used for special ops (bcast, reduction, multicast) when the handler field
  // is repurposed
  CmiInt2 swapHandlerId;
  bool nokeep;
  CmiUint1 zcMsgType; // 0: normal, 1: zero-copy
} CmiMessageHeader;

typedef CmiMessageHeader CmiMsgHeaderBasic;

// #define CMK_NODE_QUEUE_AVAILABLE CMK_SMP
#define CMK_NODE_QUEUE_AVAILABLE 1 // we assume CMK_SMP

typedef struct {
  int parent;
  int child_count;
  int *children;
} CmiSpanningTreeInfo;

extern CmiSpanningTreeInfo *_topoTree;

#define CMK_MULTICAST_GROUP_TYPE                                               \
  struct {                                                                     \
    unsigned pe, id;                                                           \
  }
typedef CMK_MULTICAST_GROUP_TYPE CmiGroup;

#define CmiMsgHeaderSizeBytes sizeof(CmiMessageHeader)
#define CmiExtHeaderSizeBytes CmiMsgHeaderSizeBytes
#define CmiReservedHeaderSize CmiMsgHeaderSizeBytes

typedef void (*CmiStartFn)(int argc, char **argv);
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched = 0,
                  int initret = 0);

static CmiStartFn Cmi_startfn;
extern int CharmLibInterOperate;

struct alignas(ALIGN_BYTES) CmiChunkHeader {
  int size;

private:
  std::atomic<int> ref; // only supports smp, would just be int otherwise

public:
  CmiChunkHeader() = default;
  CmiChunkHeader(const CmiChunkHeader &x) : size{x.size}, ref{x.getRef()} {}

  // reference counting (assumes SMP. otherwise could just use basic int
  // operations)
  int getRef() const { return ref.load(std::memory_order_acquire); }
  void setRef(int r) { return ref.store(r, std::memory_order_release); }
  int incRef() { return ref.fetch_add(1, std::memory_order_release); }
  int decRef() { return ref.fetch_sub(1, std::memory_order_release); }
};

// threads library

// Cth functions
#define CthCpvDeclare(t, v) CpvDeclare(t, v)
#define CthCpvExtern(t, v) CpvExtern(t, v)
#define CthCpvStatic(t, v) CpvStaticDeclare(t, v)
#define CthCpvInitialize(t, v) CpvInitialize(t, v)
#define CthCpvAccess(x) CpvAccess(x)

typedef struct CthThreadStruct *CthThread;
typedef struct {
  /*Start with a message header so threads can be enqueued
    as messages (e.g., by CthEnqueueNormalThread in convcore.C)
  */
  char cmicore[CmiMsgHeaderSizeBytes];
  CthThread thread;
  int serialNo;
} CthThreadToken;

typedef void (*CthVoidFn)(void *);
typedef void (*CthAwkFn)(CthThreadToken *, int, int prioBits,
                         unsigned int *prioptr);
typedef CthThread (*CthThFn)(void);

CthThreadToken *CthGetToken(CthThread);

void CthInit(char **argv);
void CthSchedInit();

CthThread CthSelf(void);

CthThread CthCreate(CthVoidFn fn, void *arg, int size);

static void CthThreadFree(CthThread t);

void CthFree(CthThread t);

void CthResume(CthThread t);

void CthSuspend(void);

void CthAwaken(CthThread th);

void CthAwakenPrio(CthThread th, int s, int pb, unsigned int *prio);

void CthYield(void);

void CthTraceResume(CthThread t);

void CthSetEventInfo(CthThread t, int event, int srcPE);

void CthSetStrategyDefault(CthThread t);

int CthImplemented(void);

#define CmiTurnOnStats()
#define CmiTurnOffStats()

// Ctv functions

CthCpvExtern(char *, CthData);
extern size_t CthRegister(size_t dataSize);
extern void CthRegistered(size_t dataOffMax);
extern char *CthGetData(CthThread t);

#define CtvDeclare(t, v)                                                       \
  typedef t CtvType##v;                                                        \
  CsvDeclare(int, CtvOffs##v) = (-1)
#define CtvStaticDeclare(t, v)                                                 \
  typedef t CtvType##v;                                                        \
  CsvStaticDeclare(int, CtvOffs##v) = (-1)
#define CtvExtern(t, v)                                                        \
  typedef t CtvType##v;                                                        \
  CsvExtern(int, CtvOffs##v)

#define CtvAccess(v)                                                           \
  (*((CtvType##v *)(CthCpvAccess(CthData) + CsvAccess(CtvOffs##v))))
#define CtvAccessOther(t, v)                                                   \
  (*((CtvType##v *)(CthGetData(t) + CsvAccess(CtvOffs##v))))
#define CtvInitialize(t, v)                                                    \
  do {                                                                         \
    if (CsvAccess(CtvOffs##v) == (-1))                                         \
      CsvAccess(CtvOffs##v) = CthRegister(sizeof(CtvType##v));                 \
    else                                                                       \
      CthRegistered(CsvAccess(CtvOffs##v) + sizeof(CtvType##v));               \
  } while (0)

#define CtvInitialized(v) (CsvAccess(CtvOffs##v) != (-1))

/* Given a user chunk m, extract the enclosing chunk header fields: */
#define BLKSTART(m) ((CmiChunkHeader *)(((intptr_t)m) - sizeof(CmiChunkHeader)))
#define SIZEFIELD(m) ((BLKSTART(m))->size)
#define REFFIELD(m) ((BLKSTART(m))->getRef())
#define REFFIELDSET(m, r) ((BLKSTART(m))->setRef(r))
#define REFFIELDINC(m) ((BLKSTART(m))->incRef())
#define REFFIELDDEC(m) ((BLKSTART(m))->decRef())

int CmiGetReference(void *blk);
void CmiReference(void *blk);
int CmiSize(void *blk);

#define CmiTmpAlloc(size) malloc(size)
#define CmiTmpFree(ptr) free(ptr)

enum ncpyRegModes {
  CMK_BUFFER_REG = 0,
  CMK_BUFFER_UNREG = 1,
  CMK_BUFFER_PREREG = 2,
  CMK_BUFFER_NOREG = 3
};

enum ncpyDeregModes { CMK_BUFFER_DEREG = 4, CMK_BUFFER_NODEREG = 5 };

// handler tools
typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr);
int CmiRegisterHandler(CmiHandler h);
int CmiRegisterHandlerEx(CmiHandlerEx h, void *userPtr);
CmiHandler CmiHandlerToFunction(int handlerId);
int CmiGetInfo(void *msg);
void CmiSetInfo(void *msg, int infofn);
void CmiNumberHandler(int n, CmiHandler h);
void CmiNumberHandlerEx(int n, CmiHandlerEx h, void *userPtr);

// message allocation/memory
void *CmiAlloc(int size);
void CmiFree(void *msg);
#define CmiRdmaAlloc CmiAlloc
#define CmiRdmaFree CmiFree
#define CmiMemoryUsage() 0
void CmiMemoryMarkBlock(void *blk);
extern void
    *memory_stack_top; // TODO: replace this with actual memory implementation

// state getters
int CmiMyPe();
int CmiMyNode();
int CmiMyNodeSize();
int CmiMyRank();
int CmiNumPes();
int CmiNumNodes();
// FIXME
//#define CmiPhysicalNodeID(node) (node)
extern int CmiPhysicalNodeID(int pe);
int CmiNodeOf(int pe);
int CmiRankOf(int pe);
int CmiStopFlag();
#define CmiNodeSize(n) (CmiMyNodeSize())
int CmiNodeFirst(int node);

// handler things
void CmiSetHandler(void *msg, int handlerId);
void CmiSetXHandler(void *msg, int xhandlerId);
int CmiGetHandler(void *msg);
int CmiGetXHandler(void *msg);
CmiHandler CmiGetHandlerFunction(void *msg);
void CmiHandleMessage(void *msg);

// message sending
void CmiSyncSend(int destPE, int messageSize, void *msg);
void CmiSyncSendAndFree(int destPE, int messageSize, void *msg);
void CmiSyncListSend(int npes, const int *pes, int len, void *msg);
void CmiSyncListSendAndFree(int npes, const int *pes, int len, void *msg);
void CmiPushPE(int destPE, void *msg);

void CmiSyncSendFn(int destPE, int messageSize, char *msg);
void CmiFreeSendFn(int destPE, int messageSize, char *msg);
void CmiSyncListSendFn(int npes, const int *pes, int len, char *msg);
void CmiFreeListSendFn(int npes, const int *pes, int len, char *msg);

// broadcasts
void CmiSyncBroadcast(int size, void *msg);
void CmiSyncBroadcastAndFree(int size, void *msg);
void CmiSyncBroadcastAll(int size, void *msg);
void CmiSyncBroadcastAllAndFree(int size, void *msg);
void CmiSyncNodeSend(unsigned int destNode, unsigned int size, void *msg);
void CmiSyncNodeSendAndFree(unsigned int destNode, unsigned int size,
                            void *msg);

void CmiSyncBroadcastFn(int size, char *msg);
void CmiFreeBroadcastFn(int size, char *msg);
void CmiSyncBroadcastAllFn(int size, char *msg);
void CmiFreeBroadcastAllFn(int size, char *msg);
void CmiFreeNodeSendFn(int node, int size, char *msg);

void CmiWithinNodeBroadcast(int size, void *msg);
void CmiSyncNodeBroadcast(unsigned int size, void *msg);
void CmiSyncNodeBroadcastAndFree(unsigned int size, void *msg);
void CmiSyncNodeBroadcastAll(unsigned int size, void *msg);
void CmiSyncNodeBroadcastAllAndFree(unsigned int size, void *msg);

// multicast and group
CmiGroup CmiEstablishGroup(int npes, int *pes);
void CmiLookupGroup(CmiGroup grp, int *npes, int **pes);
void CmiSyncMulticast(CmiGroup grp, int size, void *msg);
void CmiSyncMulticastAndFree(CmiGroup grp, int size, void *msg);
void CmiSyncMulticastFn(CmiGroup grp, int size, char *msg);
void CmiFreeMulticastFn(CmiGroup grp, int size, char *msg);

// Barrier functions
void CmiNodeBarrier();
void CmiNodeAllBarrier();
#define CmiBarrier() CmiNodeBarrier()

typedef pthread_mutex_t *CmiNodeLock;
typedef CmiNodeLock CmiImmediateLockType;
extern int _immediateLock;
extern int _immediateFlag;
extern CmiNodeLock _smp_mutex;

// scheduler
void CsdExitScheduler();
int CsdScheduler(int maxmsgs);
//void CsdEnqueueGeneral(void *Message, int strategy, int priobits, int *prioptr);
//void CsdNodeEnqueueGeneral(void *Message, int strategy, int priobits,
                          // unsigned int *prioptr);

// Message-priority pair for the queue
struct MessagePriorityPair {
  void* message;
  long long priority;
  
  MessagePriorityPair(void* msg, long long prio) : message(msg), priority(prio) {}
};

// Comparator for increasing order of priority values
struct MessagePriorityComparator {
  bool operator()(const MessagePriorityPair& a, const MessagePriorityPair& b) const {
    return a.priority > b.priority; // Note: inverted for min-heap behavior
  }
};

typedef std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator> *Queue;

#define QueueInit() new std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>()

CpvExtern(Queue, CsdSchedQueue);
CsvExtern(Queue, CsdNodeQueue);
CsvExtern(CmiNodeLock, CsdNodeQueueLock);
void CqsEnqueueGeneral(Queue q, void *Message, int strategy, int priobits,
                         unsigned int *prioptr);
#define CsdEnqueueGeneral(msg, strategy, priobits, prioptr) \
  (CqsEnqueueGeneral((Queue)CpvAccess(CsdSchedQueue),(msg),(strategy),(priobits),(prioptr)))
#define CsdNodeEnqueueGeneral(msg, strategy, priobits, prioptr) do { \
          CmiLock(CsvAccess(CsdNodeQueueLock)); \
          CqsEnqueueGeneral((Queue)CsvAccess(CsdNodeQueue),(msg),(strategy),(priobits),(prioptr)); \
          CmiUnlock(CsvAccess(CsdNodeQueueLock)); \
        } while(0)

void CmiAssignOnce(int *variable, int value);

// Reduction functions
typedef void *(*CmiReduceMergeFn)(int *, void *, void **, int);
void CmiReduce(void *msg, int size, CmiReduceMergeFn mergeFn);
void CmiNodeReduce(void *msg, int size, CmiReduceMergeFn mergeFn);
void CmiResetGlobalReduceSeqID();
void CmiResetGlobalNodeReduceSeqID();

// Exit functions
void CmiExit(int status);
#define ConverseExit(...) CmiExit(__VA_ARGS__ + 0)
void CmiAbort(const char *format, ...);

// Utility functions
int CmiPrintf(const char *format, ...);
int CmiGetArgc(char **argv);
void CmiAbort(const char *format, ...);
int CmiScanf(const char *format, ...);
int CmiError(const char *format, ...);
#define CmiMemcpy(dest, src, size) memcpy((dest), (src), (size))

#define setMemoryTypeChare(p) /* empty memory debugging method */
#define setMemoryTypeMessage(p)

void CmiInitCPUTopology(char **argv);
void CmiInitCPUAffinity(char **argv);

void __CmiEnforceMsgHelper(const char *expr, const char *fileName, int lineNum,
                           const char *msg, ...);

#define CmiEnforce(condition)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      __CmiEnforceMsgHelper(#condition, __FILE__, __LINE__, "");               \
    }                                                                          \
  } while (0)

double getCurrentTime(void);
double CmiWallTimer(void);
#define CmiCpuTimer() CmiWallTimer()
#define CmiTimer() CmiWallTimer()
double CmiStartTimer(void);
double CmiInitTime(void);
int CmiTimerAbsolute(void);

// rand functions that charm uses
void CrnSrand(unsigned int);
int CrnRand(void);
double CrnDrand(void);
int CrnRandRange(int, int);
double CrnDrandRange(double, double);

// convconds
#define CcdSCHEDLOOP 0
#define CcdPROCESSOR_BEGIN_BUSY 1
#define CcdPROCESSOR_END_IDLE 1 /*Synonym*/
#define CcdPROCESSOR_BEGIN_IDLE 2
#define CcdPROCESSOR_END_BUSY 2 /*Synonym*/
#define CcdPROCESSOR_STILL_IDLE 3
#define CcdPROCESSOR_LONG_IDLE 4

/*Periodic calls*/
#define CcdPERIODIC_FIRST 5      /*first periodic value*/
#define CcdPERIODIC 5            /*every few ms*/
#define CcdPERIODIC_10ms 6       /*every 10ms (100Hz)*/
#define CcdPERIODIC_100ms 7      /*every 100ms (10Hz)*/
#define CcdPERIODIC_1second 8    /*every second*/
#define CcdPERIODIC_1s 8         /*every second*/
#define CcdPERIODIC_5s 9         /*every second*/
#define CcdPERIODIC_5seconds 9   /*every second*/
#define CcdPERIODIC_10second 10  /*every 10 seconds*/
#define CcdPERIODIC_10seconds 10 /*every 10 seconds*/
#define CcdPERIODIC_10s 10       /*every 10 seconds*/
#define CcdPERIODIC_1minute 11   /*every minute*/
#define CcdPERIODIC_2minute 12   /*every 2 minute*/
#define CcdPERIODIC_5minute 13   /*every 5 minute*/
#define CcdPERIODIC_10minute 14  /*every 10 minutes*/
#define CcdPERIODIC_1hour 15     /*every hour*/
#define CcdPERIODIC_12hour 16    /*every 12 hours*/
#define CcdPERIODIC_1day 17      /*every day*/
#define CcdPERIODIC_LAST 18      /*last periodic value +1*/

/*Other conditions*/
#define CcdQUIESCENCE 18
#define CcdTOPOLOGY_AVAIL 19
#define CcdSIGUSR1 20
#define CcdSIGUSR2 21

/*User-defined conditions start here*/
#define CcdUSER 22
#define CcdUSERMAX 127

// convcond functions
typedef void (*CcdCondFn)(void *userParam);
typedef void (*CcdVoidFn)(void *userParam, double curWallTime);
void CcdModuleInit();
#define CcdIGNOREPE -2
void CcdCallFnAfter(CcdVoidFn fnp, void *arg, double msecs);
void CcdCallFnAfterOnPE(CcdVoidFn fnp, void *arg, double msecs, int pe);
int CcdCallOnCondition(int condnum, CcdCondFn fnp, void *arg);
int CcdCallOnConditionKeep(int condnum, CcdCondFn fnp, void *arg);
void CcdCancelCallOnCondition(int condnum, int idx);
void CcdCancelCallOnConditionKeep(int condnum, int idx);
void CcdRaiseCondition(int condnum);
void CcdCallBacks(void);

#define CQS_QUEUEING_FIFO 2
#define CQS_QUEUEING_LIFO 3
#define CQS_QUEUEING_IFIFO 4
#define CQS_QUEUEING_ILIFO 5
#define CQS_QUEUEING_BFIFO 6
#define CQS_QUEUEING_BLIFO 7
#define CQS_QUEUEING_LFIFO 8
#define CQS_QUEUEING_LLIFO 9

#define OBJ_ID_SZ 4
typedef struct _CmiObjId {
  int id[OBJ_ID_SZ];
  /*
   * **CWL** Note: setting initial values to -1 does not seem to be done for
   *               LDObjid. Potential consistency problems could arise. This
   *               will probably have to be dealt with later.
   */
#ifdef __cplusplus
  _CmiObjId() {
    for (int i = 0; i < OBJ_ID_SZ; i++) {
      id[i] = -1;
    }
  }
  bool isNull() {
    for (int i = 0; i < OBJ_ID_SZ; i++) {
      if (id[i] != -1)
        return false;
    }
    return true;
  }
  bool operator==(const struct _CmiObjId &objid) const {
    for (int i = 0; i < OBJ_ID_SZ; i++)
      if (id[i] != objid.id[i])
        return false;
    return true;
  }
#endif
} CmiObjId;

CmiObjId *CthGetThreadID(CthThread th);
void CthSetThreadID(CthThread th, int a, int b, int c);

struct CthThreadListener;

typedef void (*CthThreadListener_suspend)(struct CthThreadListener *l);
typedef void (*CthThreadListener_resume)(struct CthThreadListener *l);
typedef void (*CthThreadListener_free)(struct CthThreadListener *l);

struct CthThreadListener {
  CthThreadListener_suspend suspend; // This thread is about to block.
  CthThreadListener_resume
      resume; // This thread is about to begin execution after blocking.
  CthThreadListener_free free; // This thread is being destroyed.
  void *data; // Pointer to listener-specific data (if needed). Set by listener.
  CthThread thread; // Pointer to the thread this listener controls. Set by
                    // CthAddListener.
  struct CthThreadListener
      *next; // The next listener, or NULL at end of chain. Set by
             // CthAddListener, and used only by threads.C.
};

void CthAddListener(CthThread th, struct CthThreadListener *l);

/* Command-Line-Argument handling */
void CmiArgGroup(const char *parentName, const char *groupName);
int CmiGetArgInt(char **argv, const char *arg, int *optDest);
int CmiGetArgIntDesc(char **argv, const char *arg, int *optDest,
                     const char *desc);
int CmiGetArgLong(char **argv, const char *arg, CmiInt8 *optDest);
int CmiGetArgLongDesc(char **argv, const char *arg, CmiInt8 *optDest,
                      const char *desc);
int CmiGetArgDouble(char **argv, const char *arg, double *optDest);
int CmiGetArgDoubleDesc(char **argv, const char *arg, double *optDest,
                        const char *desc);
int CmiGetArgString(char **argv, const char *arg, char **optDest);
int CmiGetArgStringDesc(char **argv, const char *arg, char **optDest,
                        const char *desc);
int CmiGetArgFlag(char **argv, const char *arg);
int CmiGetArgFlagDesc(char **argv, const char *arg, const char *desc);
void CmiDeleteArgs(char **argv, int k);
int CmiGetArgc(char **argv);
char **CmiCopyArgs(char **argv);
void CmiFreeArgs(char **argv);
int CmiArgGivingUsage(void);
void CmiDeprecateArgInt(char **argv, const char *arg, const char *desc,
                        const char *warning);

#define CmiCreateImmediateLock() (0)
#define CmiImmediateLock(ignored)                                              \
  { _immediateLock++; }
#define CmiImmediateUnlock(ignored)                                            \
  { _immediateLock--; }
#define CmiCheckImmediateLock(ignored)                                         \
  ((_immediateLock) ? ((_immediateFlag = 1), 1) : 0)
#define CmiClearImmediateFlag()                                                \
  { _immediateFlag = 0; }
#define CmiBecomeImmediate(msg) /* empty */
#define CmiResetImmediate(msg)  /* empty */
#define CmiIsImmediate(msg) (0)
#define CmiImmIsRunning() (0)

CmiNodeLock CmiCreateLock();
void CmiDestroyLock(CmiNodeLock lock);
void CmiLock(CmiNodeLock lock);
void CmiUnlock(CmiNodeLock lock);
int CmiTryLock(CmiNodeLock lock);

// error checking

// do we want asserts to be defaulted to be on or off(right now it is on)
#ifndef CMK_OPTIMIZE
#define CMK_OPTIMIZE 0
#endif

#if CMK_OPTIMIZE
#define CmiAssert(expr) ((void)0)
#define CmiAssertMsg(expr, ...) ((void)0)
#else
#define CmiAssert(expr)                                                        \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "Assertion %s failed: file %s, line %d\n", #expr,        \
              __FILE__, __LINE__);                                             \
      CmiAbort("Failed assert");                                               \
    }                                                                          \
  } while (0)

#define CmiAssertMsg(expr, ...)                                                \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
      CmiAbort("Failed assert");                                               \
    }                                                                          \
  } while (0)
#endif

#define _MEMCHECK(p)                                                           \
  do {                                                                         \
    if (!p) {                                                                  \
      fprintf(stderr, "Memory allocation check failed: %s:%d\n", __FILE__,     \
              __LINE__);                                                       \
      abort();                                                                 \
    }                                                                          \
  } while (0)

// Cld

#define CLD_ANYWHERE (-1)
#define CLD_BROADCAST (-2)
#define CLD_BROADCAST_ALL (-3)

int CldRegisterInfoFn(CldInfoFn fn);
int CldRegisterPackFn(CldPackFn fn);
void CldRegisterEstimator(CldEstimator fn);
int CldEstimate(void);
const char *CldGetStrategy(void);

void CldEnqueue(int pe, void *msg, int infofn);
void CldEnqueueMulti(int npes, const int *pes, void *msg, int infofn);
void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn);
// CldNodeEnqueue enqueues a single message for a node, whereas
// CldEnqueueWithinNode enqueues a message for each PE on the node.
void CldNodeEnqueue(int node, void *msg, int infofn);
void CldEnqueueWithinNode(void *msg, int infofn);
void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn);

#define CmiImmIsRunning() (0)
#define CMI_MSG_NOKEEP(msg) ((CmiMessageHeader *)msg)->nokeep

// zerocopy

typedef struct ncpystruct {

  const void *srcPtr;
  char *srcLayerInfo;
  char *srcAck;
  const void *srcRef;
  int srcPe;
  size_t srcSize;
  short int srcLayerSize;
  short int srcAckSize;
  unsigned char srcRegMode;
  unsigned char srcDeregMode;
  unsigned char isSrcRegistered;

  const void *destPtr;
  char *destLayerInfo;
  char *destAck;
  const void *destRef;
  int destPe;
  size_t destSize;
  short int destAckSize;
  short int destLayerSize;
  unsigned char destRegMode;
  unsigned char destDeregMode;
  unsigned char isDestRegistered;
  unsigned char opMode;

  // Variables used for ack handling
  unsigned char ackMode;
  unsigned char freeMe;
  short int ncpyOpInfoSize;
  int rootNode;
  void *refPtr;

} NcpyOperationInfo;

enum ncpyOperationMode {
  CMK_DIRECT_API = 0,
  CMK_EM_API = 1,
  CMK_EM_API_SRC_ACK_INVOKE = 2,
  CMK_EM_API_DEST_ACK_INVOKE = 3,
  CMK_EM_API_REVERSE = 4,
  CMK_BCAST_EM_API = 5,
  CMK_BCAST_EM_API_REVERSE = 6,
  CMK_READONLY_BCAST = 7,
  CMK_ZC_PUP = 8
};

enum cmiZCMsgType {
  CMK_REG_NO_ZC_MSG = 0,
  CMK_ZC_P2P_SEND_MSG = 1,
  CMK_ZC_P2P_RECV_MSG = 2,
  CMK_ZC_SEND_DONE_MSG =
      3, // USED for both ZC_BCAST_SEND_DONE_MSG & ZC_P2P_SEND_DONE_MSG
  CMK_ZC_BCAST_SEND_MSG = 4,
  CMK_ZC_BCAST_RECV_MSG = 5,
  CMK_ZC_BCAST_RECV_DONE_MSG = 6,
  CMK_ZC_BCAST_RECV_ALL_DONE_MSG = 7,
  CMK_ZC_DEVICE_MSG = 8
};

enum ncpyAckMode { CMK_SRC_DEST_ACK = 0, CMK_SRC_ACK = 1, CMK_DEST_ACK = 2 };

enum ncpyFreeNcpyOpInfoMode {
  CMK_FREE_NCPYOPINFO = 0,
  CMK_DONT_FREE_NCPYOPINFO = 1
};

#define CMI_ZC_MSGTYPE(msg) ((CmiMsgHeaderBasic *)msg)->zcMsgType
#define CMI_IS_ZC_P2P(msg)                                                     \
  (CMI_ZC_MSGTYPE(msg) == CMK_ZC_P2P_SEND_MSG ||                               \
   CMI_ZC_MSGTYPE(msg) == CMK_ZC_P2P_RECV_MSG)
#define CMI_IS_ZC_BCAST(msg)                                                   \
  (CMI_ZC_MSGTYPE(msg) == CMK_ZC_BCAST_SEND_MSG ||                             \
   CMI_ZC_MSGTYPE(msg) == CMK_ZC_BCAST_RECV_MSG)
#define CMI_IS_ZC_RECV(msg)                                                    \
  (CMI_ZC_MSGTYPE(msg) == CMK_ZC_P2P_RECV_MSG ||                               \
   CMI_ZC_MSGTYPE(msg) == CMK_ZC_BCAST_RECV_MSG)
#define CMI_IS_ZC(msg) (CMI_IS_ZC_P2P(msg) || CMI_IS_ZC_BCAST(msg))
#define CMI_IS_ZC_DEVICE(msg) (CMI_ZC_MSGTYPE(msg) == CMK_ZC_DEVICE_MSG)

// queues
#define CQS_QUEUEING_FIFO 2
#define CQS_QUEUEING_LIFO 3
#define CQS_QUEUEING_IFIFO 4
#define CQS_QUEUEING_ILIFO 5
#define CQS_QUEUEING_BFIFO 6
#define CQS_QUEUEING_BLIFO 7
#define CQS_QUEUEING_LFIFO 8
#define CQS_QUEUEING_LLIFO 9

// libc internals
#if defined __cplusplus && defined __THROW
#define CMK_THROW __THROW
#else
#define CMK_THROW
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0 // Compatibility with non-clang compilers.
#endif
#ifndef __has_attribute
#define __has_attribute(x) 0 // Compatibility with non-clang compilers.
#endif

#if (defined __GNUC__ || __has_builtin(__builtin_unreachable)) &&              \
    !defined _CRAYC
// Technically GCC 4.5 is the minimum for this feature, but we require C++11.
#define CMI_UNREACHABLE_SECTION(...) __builtin_unreachable()
#elif _MSC_VER
#define CMI_UNREACHABLE_SECTION(...) __assume(0)
#else
#define CMI_UNREACHABLE_SECTION(...) __VA_ARGS__
#endif

#define CMI_NORETURN_FUNCTION_END CMI_UNREACHABLE_SECTION(while (1));

#if defined __cplusplus
#define CMK_NORETURN [[noreturn]]
#else
#if defined _Noreturn
#define CMK_NORETURN _Noreturn
#elif defined _MSC_VER && 1200 <= _MSC_VER
#define CMK_NORETURN __declspec(noreturn)
#else
#define CMK_NORETURN __attribute__((__noreturn__))
#endif
#endif

// must be placed before return type and at both declaration and definition
#if defined __GNUC__ && __GNUC__ >= 4
#define CMI_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#elif defined _MSC_VER && _MSC_VER >= 1700
#define CMI_WARN_UNUSED_RESULT _Check_return_
#else
#define CMI_WARN_UNUSED_RESULT
#endif

#if defined __cplusplus && __cplusplus >= 201402L
#define CMK_DEPRECATED_MSG(x) [[deprecated(x)]]
#define CMK_DEPRECATED [[deprecated]]
#elif defined __GNUC__ || defined __clang__
#define CMK_DEPRECATED_MSG(x) __attribute__((deprecated(x)))
#define CMK_DEPRECATED __attribute__((deprecated))
#elif defined _MSC_VER
#define CMK_DEPRECATED_MSG(x) __declspec(deprecated(x))
#define CMK_DEPRECATED __declspec(deprecated)
#else
#define CMK_DEPRECATED_MSG(x)
#define CMK_DEPRECATED
#endif

#if __has_builtin(__builtin_expect) || (defined __GNUC__ && __GNUC__ >= 3) ||  \
    (defined __INTEL_COMPILER && __INTEL_COMPILER >= 800) ||                   \
    (defined __ibmxl__ && __ibmxl_version__ >= 10) ||                          \
    (defined __xlC__ && __xlC__ >= (10 << 8)) ||                               \
    (defined _CRAYC && _RELEASE_MAJOR >= 8) || defined __clang__
#define CMI_LIKELY(x) __builtin_expect(!!(x), 1)
#define CMI_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define CMI_LIKELY(x) (x)
#define CMI_UNLIKELY(x) (x)
#endif

#if __has_attribute(noinline) || defined __GNUC__ ||                           \
    defined __INTEL_COMPILER || defined __ibmxl__ || defined __xlC__
#define CMI_NOINLINE __attribute__((noinline))
#elif defined _MSC_VER
#define CMI_NOINLINE __declspec(noinline)
#elif defined __PGI
#define CMI_NOINLINE _Pragma("noinline")
#else
#define CMI_NOINLINE
#endif

// spantree
// later: fix the naming of these macros to be clearer
#define CMK_SPANTREE_MAXSPAN 4
#define CST_W (CMK_SPANTREE_MAXSPAN)
#define CST_NN (CmiNumNodes())
#define CmiNodeSpanTreeParent(n) ((n) ? (((n)-1) / CST_W) : (-1))
#define CmiNodeSpanTreeChildren(n, c)                                          \
  do {                                                                         \
    int _i;                                                                    \
    for (_i = 0; _i < CST_W; _i++) {                                           \
      int _x = (n)*CST_W + _i + 1;                                             \
      if (_x < CST_NN)                                                         \
        (c)[_i] = _x;                                                          \
    }                                                                          \
  } while (0)
#define CmiNumNodeSpanTreeChildren(n)                                          \
  ((((n) + 1) * CST_W < CST_NN)                                                \
       ? CST_W                                                                 \
       : ((((n)*CST_W + 1) >= CST_NN) ? 0 : ((CST_NN - 1) - (n)*CST_W)))
#define CST_R(p) (CmiRankOf(p))
#define CST_NF(n) (CmiNodeFirst(n))
#define CST_SP(n) (CmiNodeSpanTreeParent(n))
#define CST_ND(p) (CmiNodeOf(p))
#define CST_NS(p) (CmiNodeSize(CST_ND(p)))
#define CmiSpanTreeParent(p)                                                   \
  ((p) ? (CST_R(p) ? (CST_NF(CST_ND(p)) + (CST_R(p) - 1) / CST_W)              \
                   : CST_NF(CST_SP(CST_ND(p))))                                \
       : (-1))
#define CST_C(p)                                                               \
  (((CST_R(p) + 1) * CST_W < CST_NS(p))                                        \
       ? CST_W                                                                 \
       : (((CST_R(p) * CST_W + 1) >= CST_NS(p))                                \
              ? 0                                                              \
              : ((CST_NS(p) - 1) - CST_R(p) * CST_W)))
#define CST_SC(p) (CmiNumNodeSpanTreeChildren(CST_ND(p)))
#define CmiNumSpanTreeChildren(p) (CST_R(p) ? CST_C(p) : (CST_SC(p) + CST_C(p)))
#define CmiSpanTreeChildren(p, c)                                              \
  do {                                                                         \
    int _i, _c = 0;                                                            \
    if (CST_R(p) == 0) {                                                       \
      for (_i = 0; _i < CST_W; _i++) {                                         \
        int _x = CST_ND(p) * CST_W + _i + 1;                                   \
        if (_x < CST_NN)                                                       \
          (c)[_c++] = CST_NF(_x);                                              \
      }                                                                        \
    }                                                                          \
    for (_i = 0; _i < CST_W; _i++) {                                           \
      int _x = CST_R(p) * CST_W + _i + 1;                                      \
      if (_x < CST_NS(p))                                                      \
        (c)[_c++] = CST_NF(CST_ND(p)) + _x;                                    \
    }                                                                          \
  } while (0)

extern void CsdSchedulePoll(void);

// topology
extern int CmiNumCores(void);
extern int CmiCpuTopologyEnabled(void);
extern int CmiPeOnSamePhysicalNode(int pe1, int pe2);
extern int CmiNumPesOnPhysicalNode(int node);
extern void CmiGetPesOnPhysicalNode(int node, int **pelist, int *num);
extern int CmiPhysicalRank(int pe);
extern void CmiInitCPUAffinity(char **argv);
extern int CmiPrintCPUAffinity(void);
extern int CmiSetCPUAffinity(int core);
extern int CmiSetCPUAffinityLogical(int core);
extern int CmiOnCore(void);

int CmiNumPhysicalNodes();
int CmiGetFirstPeOnPhysicalNode(int node);

static char *CopyMsg(char *msg, int len);
void CmiForwardMsgToPeers(int size, char *msg);

void LBTopoInit();

extern "C" {

size_t CmiFwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
CmiInt8 CmiPwrite(int fd, const char *buf, size_t bytes, size_t offset);
int CmiOpen(const char *pathname, int flags, int mode);
FILE *CmiFopen(const char *path, const char *mode);
int CmiFclose(FILE *fp);
}

void registerTraceInit(void (*fn)(char **argv));

int CmiDeliverMsgs(int maxmsgs);

#define CmiMemoryReadFence()                 std::atomic_thread_fence(std::memory_order_seq_cst)
#define CmiMemoryWriteFence()                std::atomic_thread_fence(std::memory_order_seq_cst)

extern CmiNodeLock CmiMemLock_lock;
#define CmiMemLock() do{if (CmiMemLock_lock) CmiLock(CmiMemLock_lock);} while (0)

#define CmiMemUnlock() do{if (CmiMemLock_lock) CmiUnlock(CmiMemLock_lock);} while (0)

template <typename T> struct CmiIsAtomic : std::false_type {};
template <typename T> struct CmiIsAtomic<std::atomic<T>> : std::true_type {};

template <typename T>
typename std::enable_if<CmiIsAtomic<T>::value, typename T::value_type>::type
CmiAtomicFetchAndIncImpl(T& input) {
    return std::atomic_fetch_add(&input, typename T::value_type(1));
}

template <typename T>
typename std::enable_if<!CmiIsAtomic<T>::value, T>::type
CmiAtomicFetchAndIncImpl(T& input) {
    T old = input;
    ++input;
    return old;
}

#define CmiMemoryAtomicFetchAndInc(input, output) ((output) = CmiAtomicFetchAndIncImpl(input))

#define CmiEnableUrgentSend(yn) /* intentionally left empty */

typedef struct CmmTableStruct *CmmTable;

#define CmmWildCard (-1)

//typedef void (*CmmPupMessageFn)(pup_er p,void **msg);
//CmmTable CmmPup(pup_er p, CmmTable t, CmmPupMessageFn msgpup);

CmmTable   CmmNew(void);
void       CmmFree(CmmTable t);
void	   CmmFreeAll(CmmTable t);
void       CmmPut(CmmTable t, int ntags, int *tags, void *msg);
void      *CmmFind(CmmTable t, int ntags, int *tags, int *returntags, int del);
int        CmmEntries(CmmTable t);
int 	   CmmGetLastTag(CmmTable t, int ntags, int *tags);
#define    CmmGet(t,nt,tg,rt)   (CmmFind((t),(nt),(tg),(rt),1))
#define    CmmProbe(t,nt,tg,rt) (CmmFind((t),(nt),(tg),(rt),0))


#ifndef CMI_CACHE_LINE_SIZE
#ifdef __cpp_lib_hardware_interference_size
# define CMI_CACHE_LINE_SIZE std::hardware_destructive_interference_size
#elif CMK_PPC64 || (defined __APPLE__ && defined __arm64__)
# define CMI_CACHE_LINE_SIZE 128
#else
# define CMI_CACHE_LINE_SIZE 64
#endif
#endif
//partitions

typedef enum Partition_Type {
  PARTITION_SINGLETON,
  PARTITION_DEFAULT,
  PARTITION_MASTER,
  PARTITION_PREFIX
} Partition_Type;

/* variables and functions for partition */
typedef struct {
  Partition_Type type;
  int isTopoaware, scheme;
  int numPartitions;
  int *partitionSize;
  int *partitionPrefix;
  int *nodeMap;
  int myPartition;
  char *partsizes;
} PartitionInfo;

void CmiCreatePartitions(char **argv);
void CmiSetNumPartitions(int nump);
void CmiSetMasterPartition(void);
void CmiSetPartitionSizes(char *size);
void CmiSetPartitionScheme(int scheme);
void CmiSetCustomPartitioning(void);

extern int _Cmi_mype_global;
extern int _Cmi_numpes_global;
extern int _Cmi_mynode_global;
extern int _Cmi_numnodes_global;
extern PartitionInfo _partitionInfo;

#define CmiNumPartitions() _partitionInfo.numPartitions
#define CmiMyPartition() _partitionInfo.myPartition
#define CmiPartitionSize(part) _partitionInfo.partitionSize[part]
#define CmiMyPartitionSize() CmiPartitionSize(CmiMyPartition())
#define CmiNumNodesGlobal() _Cmi_numnodes_global
#define CmiMyNodeGlobal() _Cmi_mynode_global
#define CmiNumPesGlobal() _Cmi_numpes_global
/* we need different implementations of this based on SMP or non-smp */
extern int CmiMyPeGlobal(void);

/* functions to translate between local and global */
int node_lToGTranslate(int node, int partition);
int pe_lToGTranslate(int pe, int partition);

#define CmiGetPeGlobal(pe, part) pe_lToGTranslate(pe, part)
#define CmiGetNodeGlobal(node, part) node_lToGTranslate(node, part)
#define CmiGetPeLocal(pe) pe_gToLTranslate(pe)
#define CmiGetNodeLocal(node) node_gToLTranslate(node)

void CmiInterSyncSend(int destPE, int partition, int messageSize, void *msg);
void CmiInterSyncSendAndFree(int destPE, int partition, int messageSize,
                             void *msg);
void CmiInterSyncNodeSend(int destNode, int partition, int messageSize,
                          void *msg);
void CmiInterSyncNodeSendAndFree(int destNode, int partition, int messageSize,
                                 void *msg);
void CmiInterSyncSendFn(int destPE, int partition, int messageSize, char *msg);
void CmiInterFreeSendFn(int destPE, int partition, int messageSize, char *msg);
void CmiInterSyncNodeSendFn(int destNode, int partition, int messageSize,
                            char *msg);
void CmiInterSyncNodeSendAndFreeFn(int destNode, int partition, int messageSize,
                                   char *msg);

/* end of variables and functions for partition */

#endif // CONVERSE_H
