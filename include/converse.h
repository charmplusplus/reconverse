// function declarations that a user program can call

#ifndef CONVERSE_H
#define CONVERSE_H

// note: this file is included by both C and C++ files, so it should always have valid C code
// CMI_MSVC: the Microsoft compiler proper, which lacks the GCC builtins,
// __thread, __int128, and (in C) usable <stdatomic.h>. clang-cl defines
// _MSC_VER too but speaks GCC, so it takes the GCC paths.
#if defined(_MSC_VER) && !defined(__clang__)
#define CMI_MSVC 1
#else
#define CMI_MSVC 0
#endif

#ifdef __cplusplus
#include <atomic>
#include <queue>
#include <utility>
#elif !CMI_MSVC
#include <stdatomic.h>
#endif
#if CMI_MSVC
#include <intrin.h>
#endif
#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#ifdef __cplusplus
#  define CLINKAGE extern "C"
#else
#  define CLINKAGE /*empty*/
#endif

/* Everything a C program can call is declared with C linkage, so that a C
   program links against the library. C++-only declarations (templates,
   overloads, classes) sit between the blocks. */
#ifdef __cplusplus
extern "C" {
#endif

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
// Only where the compiler has a 128-bit integer; Charm++ guards its uses of
// these with CMK_HAS_INT16, which it detects the same way.
#ifdef __SIZEOF_INT128__
typedef __int128_t CmiInt16;
typedef __uint128_t CmiUInt16;
#endif

#ifdef _WIN32
// A Windows SRW lock. Named by the struct tag <windows.h> gives SRWLOCK, so
// that this header does not have to include <windows.h> itself.
typedef struct _RTL_SRWLOCK *CmiNodeLock;
#else
typedef pthread_mutex_t *CmiNodeLock;
#endif
typedef CmiNodeLock CmiImmediateLockType;
extern int _immediateLock;
extern int _immediateFlag;
extern CmiNodeLock _smp_mutex;

extern CmiNodeLock CmiMemLock_lock;
#define CmiMemLock() do{if (CmiMemLock_lock) CmiLock(CmiMemLock_lock);} while (0)

#define CmiMemUnlock() do{if (CmiMemLock_lock) CmiUnlock(CmiMemLock_lock);} while (0)

#define CmiInCommThread()  (0)

#define CMK_TAG(x, y) x##y##_
#define CMK_CONCAT(x, y) x##y

//used in charm files such as ck128bitHash.h
#if defined _MSC_VER
# define CMI_FORCE_INLINE __forceinline
#elif defined __GNUC__
# define CMI_FORCE_INLINE inline __attribute__((always_inline))
#else
# define CMI_FORCE_INLINE inline
#endif

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

#ifdef __cplusplus
/* In C++, use new so t's constructor gets called */
# define CpvInit_Alloc(t,n) new t[n]()
# define CpvInit_Alloc_scalar(t) new t()
#else
# define CpvInit_Alloc(t,n) (t *)calloc(n,sizeof(t))
# define CpvInit_Alloc_scalar(t) (t *)calloc(1,sizeof(t))
#endif

// Storage class of the per-PE pointer behind each Cpv. Deliberately __thread
// rather than C++ thread_local where the compiler has it: an extern
// thread_local is reached through a TLS wrapper call, and CpvAccess is on
// every hot path. MSVC's __declspec(thread) is the same kind of plain,
// constant-initialized TLS slot.
#if CMI_MSVC
#define CMI_CPV_TLS __declspec(thread)
#else
#define CMI_CPV_TLS __thread
#endif

#define CpvDeclare(t,v) CMI_CPV_TLS t* CMK_TAG(Cpv_,v) = NULL;   \
                        int CMK_TAG(Cpv_inited_,v) = 0;  \
                        t ** CMK_TAG(Cpv_addr_,v)
#define CpvExtern(t,v)  extern CMI_CPV_TLS t* CMK_TAG(Cpv_,v);  \
                        extern int CMK_TAG(Cpv_inited_,v);  \
                        extern t ** CMK_TAG(Cpv_addr_,v)
#ifdef __cplusplus
#define CpvCExtern(t,v) extern "C" CMI_CPV_TLS t* CMK_TAG(Cpv_,v);  \
                        extern "C" int CMK_TAG(Cpv_inited_,v);  \
                        extern "C" t ** CMK_TAG(Cpv_addr_,v)
#else
#define CpvCExtern(t,v)    CpvExtern(t,v)
#endif
#define CpvStaticDeclare(t,v) static CMI_CPV_TLS t* CMK_TAG(Cpv_,v) = NULL;   \
                        static int CMK_TAG(Cpv_inited_,v) = 0;  \
                        static t ** CMK_TAG(Cpv_addr_,v)

#define CpvInitialize(t,v)\
    do {                                                               \
      CmiMemLock();                                                    \
      if (!(CMK_TAG(Cpv_inited_,v))) {                                \
        t** cpvinitobj = CpvInit_Alloc(t*, CmiMyNodeSize());         \
        CMK_TAG(Cpv_addr_,v) = cpvinitobj;                             \
        CMK_TAG(Cpv_inited_,v) = 1;                                    \
        CmiMemUnlock();                                                \
      }                                                                \
      else                                                             \
        CmiMemUnlock();                                                \
      t* cpvobj = CpvInit_Alloc_scalar(t);                             \
      CMK_TAG(Cpv_,v) = cpvobj;                                        \
      CMK_TAG(Cpv_addr_,v)[CmiMyRank()] = CMK_TAG(Cpv_,v);             \
    } while(0)

#define CpvAccess(v) (*CMK_TAG(Cpv_,v))
#define CpvAccessOther(v, r) (*(CMK_TAG(Cpv_addr_,v)[r]))
#define CpvInitialized(v) (0 != CMK_TAG(Cpv_, v))

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

// Size of the unit of cache coherence. Data that one PE writes often must not
// share one of these with data that other PEs read on their hot path, or every
// write invalidates the readers' copy. Apple silicon and POWER use 128-byte
// lines; x86 and the other aarch64 parts reconverse runs on use 64.
// Deliberately not std::hardware_destructive_interference_size: it does not
// exist in C (CsdNodeQueueLen_t below is spelled in both languages and both
// spellings must agree), and its value follows -mtune, so translation units
// built with different flags could disagree. ConverseInit warns if the machine
// reports a larger line than this. Defined only if not already set, so a build
// can override it (-DCMI_CACHE_LINE_SIZE=n).
#ifndef CMI_CACHE_LINE_SIZE
#if CMK_PPC64 || (defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)))
#define CMI_CACHE_LINE_SIZE 128
#else
#define CMI_CACHE_LINE_SIZE 64
#endif
#endif

// Portable alignment specifier for structs.
// NOTE: __attribute__((aligned(n))) must be checked before _Alignas because
// _Alignas is a variable specifier in C11/C17 and cannot appear between
// 'struct' and the tag name, whereas __attribute__ can (GCC/Clang extension).
#ifdef __cplusplus
#define CMI_ALIGNAS(n) alignas(n)
#elif CMI_MSVC
// Also checked before _Alignas: MSVC in /std:c11 claims C11 but rejects
// _Alignas in the 'struct X tag' position; __declspec(align) is accepted there.
#define CMI_ALIGNAS(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
// GCC/Clang __attribute__ supports the 'struct __attribute__((aligned(n))) tag' form
#define CMI_ALIGNAS(n) __attribute__((aligned(n)))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
// C23 has alignas
#define CMI_ALIGNAS(n) alignas(n)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
// C11 has _Alignas
#define CMI_ALIGNAS(n) _Alignas(n)
#elif defined(_MSC_VER)
// MSVC
#define CMI_ALIGNAS(n) __declspec(align(n))
#else
// Fallback - no alignment (may cause issues on some platforms)
#define CMI_ALIGNAS(n)
#endif

// End of NOTE

// Portable thread-local storage specifier.
// NOTE: 'thread_local' is a keyword in C++11 and in C23, but in C11/C17 the
// keyword is spelled '_Thread_local' ('thread_local' is only a macro provided
// by <threads.h>).  This header is included by C sources that are compiled as
// C17 (GKlib, for instance), so the bare keyword cannot be used here.
#ifdef __cplusplus
#define CMI_THREAD_LOCAL thread_local
#elif CMI_MSVC
#define CMI_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
// C23 has thread_local
#define CMI_THREAD_LOCAL thread_local
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
// C11 has _Thread_local
#define CMI_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define CMI_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
// MSVC
#define CMI_THREAD_LOCAL __declspec(thread)
#else
// Fallback - no thread-local storage (may cause issues on some platforms)
#define CMI_THREAD_LOCAL
#endif

typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr); // ignore for now

typedef void (*CldPackFn)(void *msg);

typedef void (*CldInfoFn)(void *msg, CldPackFn *packer, int *len, int *queueing,
                          int *priobits, unsigned int **prioptr);

typedef int (*CldEstimator)(void);

#ifdef __cplusplus
#define CmiMessageDestPENode static_cast<CmiUInt4>(-1)
#else
#define CmiMessageDestPENode ((CmiUInt4)(-1))
#endif

typedef struct Header {
  CmiInt2 handlerId;
  // Seed balancer (Cld): the info function's handler index, set by CmiSetInfo.
  // It has a slot of its own because a Cld message must carry it all the way to
  // the receiving PE, including across a broadcast, and the spanning tree
  // broadcast owns collectiveMetaInfo for that whole trip. This sits in what
  // was padding after handlerId, so the header does not grow for it.
  CmiInt2 cldInfoFn;
  CmiUInt4 destPE; // global ID of destination PE
  int messageSize;
  // used for bcast (the node the broadcast is rooted at), multicast (group id),
  // reductions (reduction id)
  CmiUInt4 collectiveMetaInfo;
  // used for special ops (reduction, multicast) when the handler field is
  // repurposed. This is CmiSetXHandler/CmiGetXHandler, where the seed balancer
  // parks the user's handler (CldSwitchHandler in cldb.cpp).
  CmiInt2 swapHandlerId;
  // Where the spanning tree broadcast parks the message's real handler while
  // handlerId routes to the relay handler. It cannot share swapHandlerId: on a
  // CldEnqueue(CLD_BROADCAST*) message the seed balancer has already parked the
  // user's handler there, and overwriting it sends the message back to
  // CldHandler forever. The fan-out path avoids the same collision by putting
  // the payload's handler in its trailer (see collectives.cpp).
  CmiInt2 bcastSwapHandlerId;
  bool nokeep;
  CmiUInt1 zcMsgType; // 0: normal, 1: zero-copy
  // Set by CmiSyncSendAndFreeUrgent: the destination PE serves this message
  // from its urgent queue, polled before every other queue. Charm++ sends its
  // [expedited] entries this way. Without it an expedited message waited
  // behind everything ahead of it in the PE's FIFO: ~140 ms for a migration
  // payload mid-step on leanmd.
  CmiUInt1 urgent;
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
#ifdef __cplusplus
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched = 0,
                  int initret = 0);
#else
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched,
                  int initret);
#endif

static CmiStartFn Cmi_startfn;
extern int CharmLibInterOperate;

#ifdef __cplusplus
struct alignas(ALIGN_BYTES) CmiChunkHeader {
  int size;
  void* mr;
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
  // acq_rel, not release: the caller that drops the last reference frees the
  // block, and must observe every other holder's accesses to it first.
  int decRef() { return ref.fetch_sub(1, std::memory_order_acq_rel); }
};
#else
struct CMI_ALIGNAS(ALIGN_BYTES) CmiChunkHeader {
  int size;
  int ref; // basic int for C compilation
};

/* C functions for reference counting */
static inline int CmiChunkHeader_getRef(struct CmiChunkHeader* hdr) { return hdr->ref; }
static inline void CmiChunkHeader_setRef(struct CmiChunkHeader* hdr, int r) { hdr->ref = r; }
#if CMI_MSVC
static inline int CmiChunkHeader_incRef(struct CmiChunkHeader* hdr) { return _InterlockedExchangeAdd((volatile long *)&hdr->ref, 1); }
static inline int CmiChunkHeader_decRef(struct CmiChunkHeader* hdr) { return _InterlockedExchangeAdd((volatile long *)&hdr->ref, -1); }
#else
static inline int CmiChunkHeader_incRef(struct CmiChunkHeader* hdr) { return __atomic_fetch_add(&hdr->ref, 1, __ATOMIC_RELEASE); }
static inline int CmiChunkHeader_decRef(struct CmiChunkHeader* hdr) { return __atomic_fetch_sub(&hdr->ref, 1, __ATOMIC_ACQ_REL); }
#endif
#endif

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

int CthIsSuspendable(CthThread t);

void CthSuspend(void);
/* A spare link in each thread, for the runtime's own waiting lists (the Cth
   standby list, Cfuture waiters); a thread is on at most one such list. */
void CthSetNext(CthThread t, CthThread next);
CthThread CthGetNext(CthThread t);

/* Converse futures: a single-assignment cell a Cth thread can block on.
   The handle may be copied to any PE; CfutureSet may run on any PE,
   CfutureWait and CfutureDestroy only on the PE that created the future.
   Documented in the Converse extensions manual (section Futures). */
struct CfutureCell;
typedef struct Cfuture_s {
  int pe;
  struct CfutureCell *data;
} Cfuture;
Cfuture CfutureCreate(void);
void CfutureSet(Cfuture f, void *value, int len);
void *CfutureWait(Cfuture f);
void CfutureDestroy(Cfuture f);
/* Zero-copy form: fill the buffer, then store it; the buffer becomes the
   future's value and is freed by CfutureDestroy. */
void *CfutureCreateBuffer(int bytes);
void CfutureDestroyBuffer(void *value);
void CfutureStoreBuffer(Cfuture f, void *value);
#define CfuturePE(f) ((f).pe)

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

/* ---------------- Isomalloc and migratable threads ---------------- */

/* Opaque handle to an Isomalloc context: a heap whose allocations sit at the
   same virtual addresses in every process of the job, so that a thread using
   one can be moved between them without rewriting any pointer.

   Reconverse never creates a context and does not implement Isomalloc. A
   provider (Charm++'s isomalloc) registers itself through
   CthRegisterIsomallocOps below; without one, migratable threads degrade to
   ordinary threads with malloc'd stacks and CthMigratable() answers 0. */
#ifndef CMI_ISOMALLOC_CONTEXT_DEFINED
#define CMI_ISOMALLOC_CONTEXT_DEFINED 1
typedef struct CmiIsomallocContext {
  void *opaque;
} CmiIsomallocContext;
#endif

/* A byte-stream serializer supplied by the caller.

   Moving a thread means serializing it, and Reconverse has no serialization
   framework of its own -- the one in use belongs to the layer above (PUP, for
   Charm++). So the caller passes one in: `user` is its own serializer object,
   handed back verbatim to `bytes`, and `mode` says which direction this pass
   is going. */
#define CMI_PUP_SIZING (1 << 0)
#define CMI_PUP_PACKING (1 << 1)
#define CMI_PUP_UNPACKING (1 << 2)
#define CMI_PUP_DELETING (1 << 3)
typedef struct CmiPupStream {
  void *user;
  int mode;
  void (*bytes)(struct CmiPupStream *p, void *data, size_t n);
} CmiPupStream;
#define CmiPupIsSizing(p) (((p)->mode & CMI_PUP_SIZING) != 0)
#define CmiPupIsPacking(p) (((p)->mode & CMI_PUP_PACKING) != 0)
#define CmiPupIsUnpacking(p) (((p)->mode & CMI_PUP_UNPACKING) != 0)
#define CmiPupIsDeleting(p) (((p)->mode & CMI_PUP_DELETING) != 0)
#define CmiPupBytes(p, d, n) ((p)->bytes((p), (d), (n)))

/* The Isomalloc operations a migratable thread needs. Register once per
   process, before the first migratable thread is created. */
typedef struct CthIsomallocOps {
  /* Is Isomalloc usable at all? A provider that could not reserve a global
     address range answers 0, and migratable threads fall back to malloc. */
  int (*enabled)(void);
  /* Allocate from a context, never to be freed individually: thread stacks
     and thread-local segments live here and are released with the context. */
  void *(*permanentAllocAlign)(CmiIsomallocContext ctx, size_t align,
                               size_t size);
  void (*contextDelete)(CmiIsomallocContext ctx);
  void (*contextPup)(CmiPupStream *p, CmiIsomallocContext *ctx);
  /* Make this context the target of intercepted mallocs on this PE. Passing
     an empty context turns interception off. */
  void (*contextActivate)(CmiIsomallocContext ctx);
} CthIsomallocOps;
void CthRegisterIsomallocOps(const CthIsomallocOps *ops);

/* Create a thread whose stack is allocated from `ctx`, so that the thread can
   later be serialized with CthPupThread and resumed in another process. Falls
   back to CthCreate's behavior when no Isomalloc provider is registered. */
#define CMI_MIGRATABLE_THREADS_DECLARED 1
CthThread CthCreateMigratable(CthVoidFn fn, void *arg, int size,
                              CmiIsomallocContext ctx);

/* Can threads created by CthCreateMigratable actually migrate? */
int CthMigratable(void);

/* Serialize a thread. On an unpacking pass `t` is ignored and a newly
   allocated thread is returned; otherwise `t` comes back. The thread must not
   be the one running. */
CthThread CthPupThread(CmiPupStream *p, CthThread t);

CmiIsomallocContext CthGetIsomallocContext(CthThread t);

/* Record a position on a thread's stack in a form that survives migration,
   and recover a pointer from it. Offset 0 means "not on this stack". */
size_t CthStackOffset(CthThread t, char *p);
char *CthPointer(CthThread t, size_t pos);

/* Which flavor of user-level thread this build provides, and which optional
   properties they have. Callers use these to decide things they cannot
   otherwise ask about -- whether a stack address is stable across a migration,
   for instance. */
#define CMI_THREAD_IS_QT (1 << 1)
#define CMI_THREAD_IS_CONTEXT (1 << 2)
#define CMI_THREAD_IS_UJCONTEXT (1 << 3)
#define CMI_THREAD_IS_PTHREADS (1 << 4)
#define CMI_THREAD_IS_FIBERS (1 << 5)
#define CMI_THREAD_IS_ALIAS (1 << 6)
#define CMI_THREAD_IS_STACKCOPY (1 << 7)
#define CMI_THREAD_IS_TLS (1 << 8)
int CmiThreadIs(int flag);

/* Print the calling thread's stack to stderr, demangled, one frame per
   line; nSkip innermost frames are omitted (see backtrace.cpp). CmiAbort
   prints one, and so does the fatal-signal handler. */
void CmiPrintStackTrace(int nSkip);

/* Converse Thread Globals: swapping a private copy of the program's global
   variables in and out around each thread, for languages whose runtimes assume
   one set of globals per rank. Reconverse does not implement swapping -- the
   position-independent-executable approach puts a rank's globals in its
   Isomalloc heap instead, which needs no per-switch work -- so these are the
   no-op shape of that interface, kept so callers written against it compile
   and do nothing. */
typedef struct CtgGlobalStruct {
  void *data_seg;
} CtgGlobals;

#define CMI_PIC_NOP 0
#define CMI_PIC_ELFGOT 1
CpvExtern(int, CmiPICMethod);

void CtgInit(void);
size_t CtgGetSize(void);
CtgGlobals CtgCreate(void *buf);
void CtgInstall(CtgGlobals g);
void CtgUninstall(void);
CtgGlobals CtgCurrentGlobals(void);

/* Interceptions -- Isomalloc heap redirection today, thread-local storage and
   global-variable swapping when those arrive -- are on while a thread runs its
   own code and off while it is inside the runtime. These bracket the
   transitions and nest; a thread starts out deactivated, so the first Pop is
   what turns it on. */
void CthInterceptionsDeactivatePush(CthThread t);
void CthInterceptionsDeactivatePop(CthThread t);
/* Force interceptions on regardless of nesting depth, and put the depth back.
   Used to run a callback in the thread's own heap from inside the runtime. */
int CthInterceptionsTemporarilyActivateStart(CthThread t);
void CthInterceptionsTemporarilyActivateEnd(CthThread t, int old);

/* Given a user chunk m, extract the enclosing chunk header fields: */
#define BLKSTART(m) ((CmiChunkHeader *)(((intptr_t)m) - sizeof(CmiChunkHeader)))
#define SIZEFIELD(m) ((BLKSTART(m))->size)
  #define MRFIELD(m) ((BLKSTART(m))->mr)
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
int CmiRegisterHandler(CmiHandler h);
int CmiRegisterHandlerEx(CmiHandlerEx h, void *userPtr);
CmiHandler CmiHandlerToFunction(int handlerId);
int CmiGetInfo(void *msg);
void CmiSetInfo(void *msg, int infofn);
void CmiNumberHandler(int n, CmiHandler h);
void CmiNumberHandlerEx(int n, CmiHandlerEx h, void *userPtr);

// message allocation/memory
void *CmiAlloc(int size);

// Size of a virtual memory page, in bytes.
int CmiGetPageSize(void);

void CmiFree(void *msg);
#define CmiRdmaAlloc CmiAlloc
#define CmiRdmaFree CmiFree
// Memory usage of the process, in bytes (classic Converse's family).
// CmiMemoryUsage asks the first probe that answers on this platform:
//   - glibc 2.33 or later: mallinfo2, the bytes malloc has handed out and not
//     had back (in its arenas and in blocks it mapped on their own), unless
//     reconverse links jemalloc;
//   - macOS: malloc_zone_statistics, the same figure over all malloc zones,
//     unless reconverse links jemalloc; else the resident size (task_info);
//   - Windows: the committed private bytes, as classic Converse reported;
//   - otherwise: the resident size from /proc/self/statm.
// CmiMemoryUsageReporter names the probe that answered, or "none".
// CmiMaxMemoryUsageR is the peak resident size of the process since it
// started (getrusage's ru_maxrss; the peak working set on Windows).
// CmiMaxMemoryUsage and CmiMinMemoryUsage are the highest and lowest
// CmiMemoryUsage seen since the last CmiResetMaxMemory / CmiResetMinMemory.
// No malloc wrapper stands behind them: usage is sampled at each call to
// CmiMemoryUsage, CmiMaxMemoryUsage, CmiMinMemoryUsage and the resets, so a
// peak or trough between two calls is missed. The marks are per process.
CmiUInt8 CmiMemoryUsage(void);
const char *CmiMemoryUsageReporter(void);
CmiUInt8 CmiMaxMemoryUsageR(void);
CmiUInt8 CmiMaxMemoryUsage(void);
void CmiResetMaxMemory(void);
CmiUInt8 CmiMinMemoryUsage(void);
void CmiResetMinMemory(void);
void CmiMemoryMarkBlock(void *blk);
extern void
    *memory_stack_top; // TODO: replace this with actual memory implementation

#define CMI_MEMORY_IS_ISOMALLOC   (1<<1)
#define CMI_MEMORY_IS_PARANOID    (1<<2)
#define CMI_MEMORY_IS_GNU         (1<<3)
#define CMI_MEMORY_IS_GNUOLD      (1<<4)
#define CMI_MEMORY_IS_OS          (1<<5)
#define CMI_MEMORY_IS_CHARMDEBUG  (1<<6)
int CmiMemoryIs(int flag); /* return state of this flag */
void CpdSetInitializeMemory(int v); /* no-op outside charmdebug; needed by record-replay */
CLINKAGE void CmiMkdir(const char *dirName);

// state getters
int CmiMyPe();
int CmiMyNode();
int CmiMyNodeSize();
int CmiMyRank();
int CmiNumPes();
int CmiNumNodes();
// FIXME
//#define CmiPhysicalNodeID(node) (node)
CLINKAGE int CmiPhysicalNodeID(int pe);
int CmiNodeOf(int pe);
int CmiRankOf(int pe);
int CmiStopFlag();
#define CmiNodeSize(n) (CmiMyNodeSize())
int CmiNodeFirst(int node);

// handler things
void CmiSetHandler(void *msg, int handlerId);
// Prepare the header of a message that was not obtained from CmiAlloc (for
// example one on the stack) so it can be passed to a send: clears the
// zerocopy type and the nokeep flag and records the message size. Classic
// Converse programs call this before CmiSetHandler on such messages.
void CmiInitMsgHeader(void *msg, int size);
void CmiSetXHandler(void *msg, int xhandlerId);
int CmiGetHandler(void *msg);
int CmiGetXHandler(void *msg);
CmiHandler CmiGetHandlerFunction(void *msg);
void CmiHandleMessage(void *msg);

// message sending
void CmiSyncSend(int destPE, int messageSize, void *msg);
void CmiSyncSendAndFree(int destPE, int messageSize, void *msg);
void CmiSyncSendAndFreeUrgent(int destPE, int messageSize, void *msg);
void CmiPushPEUrgent(int destRank, void *msg);
void CmiSyncListSend(int npes, const int *pes, int len, void *msg);
void CmiSyncListSendAndFree(int npes, const int *pes, int len, void *msg);
void CmiPushPE(int destRank, void *msg);
void CsdEnqueue(void *msg);     /* classic scheduler-enqueue API; */
void CsdEnqueueLifo(void *msg); /* FIFO here -- see convcore.cpp (record-replay) */
void CmiPushNode(void *msg);

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

//network functions
void CmiNetworkProgress();
// Charm++'s CkNetworkProgress() / CkNetworkProgressAfter() expand to calls of
// CmiMachineProgressImpl() only when the layer declares that it has one;
// otherwise they compile to nothing and a long-running entry method never
// drives the network. Reconverse's progress is CmiNetworkProgress().
#define CMK_MACHINE_PROGRESS_DEFINED 1
void CmiMachineProgressImpl();
// ignore argument since it's only 0 in namd
#define CmiNetworkProgressAfter(p) CmiNetworkProgressAfter()

// Barrier functions
void CmiBarrier();
void CmiNodeBarrier();
void CmiNodeAllBarrier();

// scheduler
void CsdExitScheduler();
int CsdScheduler(int maxmsgs);

/* The scheduler queue: messages ordered by priority value (smaller first,
 * negative before zero before positive), FIFO within one priority value, or
 * LIFO for messages pushed with QueuePushFront. One deque per live priority
 * level, as in classic Converse's Cqs; see src/queueing.cpp for the design
 * and its tuning notes. The fields are private to queueing.cpp. */
typedef struct QueueImpl
{
  void *levels;      /* std::map<long long, std::deque<void*>>: nonzero priorities */
  void *zero;        /* std::deque<void*>: priority 0, the common case */
  int emptyLevels;   /* drained levels kept allocated for reuse */
  int size;          /* messages in the queue */
} *Queue;

void QueueInit(Queue q);
void QueueDestroy(Queue q);
int QueueEmpty(Queue q);
int QueueSize(Queue q);
/* FIFO within the priority level */
void QueuePush(Queue q, void* message, long long priority);
/* LIFO within the priority level: comes out before everything already
 * queued at that priority */
void QueuePushFront(Queue q, void* message, long long priority);
void QueuePop(Queue q);
void* QueueTop(Queue q);



CpvExtern(Queue, CsdSchedQueue);
CsvExtern(Queue, CsdNodeQueue);
CsvExtern(CmiNodeLock, CsdNodeQueueLock);
/* Number of messages sitting in CsdNodeQueue. Maintained under CsdNodeQueueLock
   but read without it, so that an idle PE can skip the lock entirely when the
   queue is empty -- see CsdScheduler().

   Spelled std::atomic<int> to C++ and _Atomic int to C, because this header is
   included by C translation units (GKlib's b64.c, for one) and std::atomic is
   not C -- the same split CmiChunkHeader uses above for its refcount, and the
   reason <stdatomic.h> is included at the top of this file. The two spellings
   describe the same object: both are int-sized, int-aligned and lock-free, and
   C11 and C++11 atomics share one ABI on every compiler this builds with.
   CsdNodeQueueLenAdd/Get hide the syntax that genuinely differs, so callers
   need no #ifdef of their own.

   The counter is wrapped in a struct of exactly one cache line
   (CMI_CACHE_LINE_SIZE in size and alignment) so that it has the line to
   itself. It is written on every node-queue enqueue and dequeue, and as a
   bare int it shared its line with read-hot globals (Cmi_startTime, read by
   every CmiWallTimer call, and the CsdNodeQueue/CsdNodeQueueLock pointers),
   so each write invalidated them on every PE. Both spellings of the
   struct have the atomic at offset 0 and the same size and alignment. */
#ifdef __cplusplus
struct alignas(CMI_CACHE_LINE_SIZE) CsdNodeQueueLen_t {
  std::atomic<int> v;
};
CsvExtern(CsdNodeQueueLen_t, CsdNodeQueueLen);
#define CsdNodeQueueLenAdd(n)                                                  \
  CsvAccess(CsdNodeQueueLen).v.fetch_add((n), std::memory_order_relaxed)
#define CsdNodeQueueLenGet()                                                   \
  CsvAccess(CsdNodeQueueLen).v.load(std::memory_order_relaxed)
#elif CMI_MSVC
/* MSVC's C has no usable <stdatomic.h>; a long is int-sized on Windows, so
   this is still the same object as the C++ spelling. */
typedef struct __declspec(align(CMI_CACHE_LINE_SIZE)) {
  volatile long v;
} CsdNodeQueueLen_t;
CsvExtern(CsdNodeQueueLen_t, CsdNodeQueueLen);
#define CsdNodeQueueLenAdd(n)                                                  \
  _InterlockedExchangeAdd(&CsvAccess(CsdNodeQueueLen).v, (n))
#define CsdNodeQueueLenGet() ((int)CsvAccess(CsdNodeQueueLen).v)
#else
typedef struct {
  _Alignas(CMI_CACHE_LINE_SIZE) _Atomic int v;
} CsdNodeQueueLen_t;
CsvExtern(CsdNodeQueueLen_t, CsdNodeQueueLen);
#define CsdNodeQueueLenAdd(n)                                                  \
  atomic_fetch_add_explicit(&CsvAccess(CsdNodeQueueLen).v, (n),                \
                            memory_order_relaxed)
#define CsdNodeQueueLenGet()                                                   \
  atomic_load_explicit(&CsvAccess(CsdNodeQueueLen).v, memory_order_relaxed)
#endif
void CqsEnqueueGeneral(Queue q, void *Message, int strategy, int priobits,
                         unsigned int *prioptr);
#define CsdEnqueueGeneral(msg, strategy, priobits, prioptr) \
  (CqsEnqueueGeneral((Queue)CpvAccess(CsdSchedQueue),(msg),(strategy),(priobits),(prioptr)))
#define CsdNodeEnqueueGeneral(msg, strategy, priobits, prioptr) do { \
          CmiLock(CsvAccess(CsdNodeQueueLock)); \
          CqsEnqueueGeneral((Queue)CsvAccess(CsdNodeQueue),(msg),(strategy),(priobits),(prioptr)); \
          CsdNodeQueueLenAdd(1); \
          CmiUnlock(CsvAccess(CsdNodeQueueLock)); \
        } while(0)

void CmiAssignOnce(int *variable, int value);

// Reduction functions
typedef void *(*CmiReduceMergeFn)(int *, void *, void **, int);
typedef CmiUInt4 CmiReductionID;
void CmiReduce(void *msg, int size, CmiReduceMergeFn mergeFn);
void CmiNodeReduce(void *msg, int size, CmiReduceMergeFn mergeFn);
/* A reduction over an explicit PE list, under an id every participant
 * obtained from CmiGetGlobalReduction() (same call sequence on every PE).
 * pes[0] receives the result; the list defines a 4-ary tree over the PEs. */
void CmiListReduce(int npes, const int *pes, void *msg, int size,
                   CmiReduceMergeFn mergeFn, CmiReductionID id);
/* An id for a reduction that only some PEs join (CmiListReduce). Every PE
 * must call this the same number of times in the same order, as at startup,
 * so that the ids agree. */
CmiReductionID CmiGetGlobalReduction(void);
void CmiResetGlobalReduceSeqID();
void CmiResetGlobalNodeReduceSeqID();

// Exit functions

void CmiExit(int status);
/* Never returns, as in classic Converse: callers (Charm++'s CkAbort, which is
   this function) end non-void functions with it, which MSVC rejects unless it
   knows. */
CMK_NORETURN void CmiAbort(const char *format, ...);

// Utility functions
CLINKAGE int CmiPrintf(const char *format, ...);
int CmiGetArgc(char **argv);
int CmiScanf(const char *format, ...);
int CmiError(const char *format, ...);

#ifdef __cplusplus
void ConverseExit(int status=0);
#else
void ConverseExit(int status);
#endif
#define CmiMemcpy(dest, src, size) memcpy((dest), (src), (size))

#define setMemoryTypeChare(p) /* empty memory debugging method */
#define setMemoryTypeMessage(p)

void CmiInitCPUTopology(char **argv);
void CmiInitCPUAffinity(char **argv);
int CmiOnCore(void);

#define __CMK_STRING(x) #x
#define __CMK_XSTRING(x) __CMK_STRING(x)

void __CmiEnforceHelper(const char* expr, const char* fileName, const char* lineNum);
void __CmiEnforceMsgHelper(const char* expr, const char* fileName,
			   const char* lineNum, const char* msg, ...);

#define CmiEnforce(expr)                                             \
  ((void)(CMI_LIKELY(expr) ? 0                                       \
                 : (__CmiEnforceHelper(__CMK_STRING(expr), __FILE__, \
                                       __CMK_XSTRING(__LINE__)),     \
                    0)))

#define CmiEnforceMsg(expr, ...)                                              \
  ((void)(CMI_LIKELY(expr)                                                    \
              ? 0                                                             \
              : (__CmiEnforceMsgHelper(__CMK_STRING(expr), __FILE__,          \
                                       __CMK_XSTRING(__LINE__), __VA_ARGS__), \
                 0)))

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
#define CCD_COND_FN_EXISTS 1 //needed for namd WorkDistrib.C
typedef void (*CcdCondFn)(void *userParam);
typedef void (*CcdVoidFn)(void *userParam, double curWallTime);
void CcdModuleInit();
#define CcdIGNOREPE -2
CLINKAGE void CcdCallFnAfter(CcdVoidFn fnp, void *arg, double msecs);
void CcdCallFnAfterOnPE(CcdVoidFn fnp, void *arg, double msecs, int pe);
int CcdCallOnCondition(int condnum, CcdCondFn fnp, void *arg);
int CcdCallOnConditionKeep(int condnum, CcdCondFn fnp, void *arg);
void CcdCancelCallOnCondition(int condnum, int idx);
void CcdCancelCallOnConditionKeep(int condnum, int idx);
void CcdRaiseCondition(int condnum);
void CcdCallBacks(void);

/* How many callbacks are timer-based.  If none are, CcdCallBacks() has no
   work to do and the scheduler can skip it outright. */
int CcdNumTimerCBs(void);

/* Countdown to the next CcdCallBacks(), maintained adaptively by
   CcdCallBacks() itself so that it fires at roughly CCD_DEFAULT_RESOLUTION. */
extern CMI_THREAD_LOCAL int _ccd_numchecks;

/* The scheduler's periodic hook.
 *
 * Calling CcdCallBacks() unconditionally on every trip round the scheduler
 * loop -- which is what this used to do -- costs a CmiWallTimer() (a clock
 * read, ~60 ns on Delta) plus a heap probe every iteration, whether or not any
 * timer callback exists.  For a program that registers none, which includes
 * every Task Bench run, that is pure overhead on the hottest loop in the
 * runtime.
 *
 * Both guards matter and they do different jobs.  The first collapses the
 * whole thing to one predictable branch when nothing is timer-based.  The
 * second bounds the cost when something is: CcdCallBacks() retunes
 * _ccd_numchecks so the clock is read about every CCD_DEFAULT_RESOLUTION
 * rather than every iteration.
 *
 * Ported from the original Converse (charm/src/conv-core/converse.h). */
#define CsdPeriodic()                                                          \
  do {                                                                         \
    if ((CcdNumTimerCBs() > 0) && (_ccd_numchecks-- <= 0)) {                   \
      CcdCallBacks();                                                          \
    }                                                                          \
  } while (0)

#define CsdResetPeriodic() (_ccd_numchecks = 0)

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
double CmiReadSize(const char *str);
void CmiDeleteArgs(char **argv, int k);
int CmiGetArgc(char **argv);
char **CmiCopyArgs(char **argv);
void CmiFreeArgs(char **argv);
int CmiArgGivingUsage(void);
void CmiDeprecateArgInt(char **argv, const char *arg, const char *desc,
                        const char *warning);

#define CmiCreateImmediateLock() CmiCreateLock()
#define CmiImmediateLock(immediateLock) CmiLock((immediateLock))
#define CmiImmediateUnlock(immediateLock) CmiUnlock((immediateLock))
#define CmiCheckImmediateLock(ignored)  (0)
#define CmiClearImmediateFlag() 
#define CmiBecomeImmediate(msg) /* empty */
#define CmiResetImmediate(msg)  /* empty */
#define CmiIsImmediate(msg) (0)
#define CmiImmIsRunning() (0)

CmiNodeLock CmiCreateLock();
void CmiDestroyLock(CmiNodeLock lock);
void CmiLock(CmiNodeLock lock);
void CmiUnlock(CmiNodeLock lock);
int CmiTryLock(CmiNodeLock lock);

//decrementToEnqueue
typedef struct DecrementToEnqueueMsg{
  unsigned int *counter;
  void *msg;
} DecrementToEnqueueMsg;

void CmiDecrementCounter(DecrementToEnqueueMsg *dteMsg);
#ifdef __cplusplus
//default set to 10
DecrementToEnqueueMsg *CmiCreateDecrementToEnqueue(void *msg, unsigned int initialCount = 10);
void CmiResetCounter(DecrementToEnqueueMsg *dteMsg, unsigned int newCount = 10);
#else
DecrementToEnqueueMsg *CmiCreateDecrementToEnqueue(void *msg, unsigned int initialCount);
void CmiResetCounter(DecrementToEnqueueMsg *dteMsg, unsigned int newCount);
#endif
void CmiFreeDecrementToEnqueue(DecrementToEnqueueMsg *dteMsg);

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

/* The nokeep flag is a promise by the sender that no receiving handler will
   retain the message past its return or modify it. In exchange the runtime may
   hand one shared buffer to several PEs of a process instead of copying it for
   each, every one of those PEs still calling CmiFree exactly once. Only the
   within-node broadcast shares; a list send's fan-out does not, because
   Charm++ also sets nokeep on packed messages and unpacks them in place. */
CLINKAGE void CmiSetMsgNokeep(void *msg, int nokeep);
CLINKAGE int CmiMsgIsNokeep(const void *msg);

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

  // ipc specific
  void* deviceRdmaOpInfo;

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
  CMK_ZC_PUP = 8,
  // A device-payload correction put. Its completion is raised on the initiator,
  // which is the sender, so the upper layer must notify the target rather than
  // resolve the operation locally.
  CMK_DEVICE_RESTAGE_PUT = 9,
  // A persistent device endpoint's one-sided put or get (CkDevicePersistent).
  // Its completion is raised on the initiator, which fires both endpoints'
  // callbacks; refPtr carries them. Passed through by the machine layer.
  CMK_DEVICE_PERSISTENT_OP = 10
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
CLINKAGE int CmiNumPesOnPhysicalNode(int node);
CLINKAGE void CmiGetPesOnPhysicalNode(int node, int **pelist, int *num);
CLINKAGE int CmiPhysicalRank(int pe);
extern void CmiInitCPUAffinity(char **argv);
// Warns or aborts if PEs on physical node 0 share a core; call after
// CmiInitCPUTopology, as Charm++'s init does.
extern void CmiCheckAffinity(void);
extern int CmiPrintCPUAffinity(void);
extern int CmiSetCPUAffinity(int core);
extern int CmiSetCPUAffinityLogical(int core);
extern int CmiOnCore(void);

CLINKAGE int CmiNumPhysicalNodes();
CLINKAGE int CmiGetFirstPeOnPhysicalNode(int node);
/* Rank of a logical node among those sharing its physical node.
 * Topology-derived; O(PEs on the physical node), so cache it. */
int CmiNodeRankOnPhysicalNode(int node);

static char *CopyMsg(char *msg, int len);
void CmiForwardMsgToPeers(int size, char *msg);

void LBTopoInit();

size_t CmiFwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
CmiInt8 CmiPwrite(int fd, const char *buf, size_t bytes, size_t offset);
int CmiOpen(const char *pathname, int flags, int mode);
FILE *CmiFopen(const char *path, const char *mode);
int CmiFclose(FILE *fp);

void registerTraceInit(void (*fn)(char **argv));

// Charm++ registers CmiIsomallocInit here. Isomalloc is not part of
// Reconverse -- it needs a serialization framework Reconverse does not have --
// so it lives above and is initialized through this hook, on every PE, after
// collectives are up (its address-range negotiation is a node reduction) and
// before CthInit, which is the first thing that could ask for a migratable
// stack.
void registerIsomallocInit(void (*fn)(char **argv));

// Record that a memory module with the given CMI_MEMORY_IS_* character has
// been installed. Reconverse tracks this for CmiMemoryIs(), which callers use
// to ask whether allocations are being intercepted.
void CmiMemoryIsSetFlag(int flag);

int CmiDeliverMsgs(int maxmsgs);

#ifdef __cplusplus
#define CmiMemoryReadFence()                 std::atomic_thread_fence(std::memory_order_seq_cst)
#define CmiMemoryWriteFence()                std::atomic_thread_fence(std::memory_order_seq_cst)
#elif CMI_MSVC && (defined(_M_X64) || defined(_M_IX86))
#define CmiMemoryReadFence()                 _mm_mfence()
#define CmiMemoryWriteFence()                _mm_mfence()
#elif CMI_MSVC
#define CmiMemoryReadFence()                 __dmb(_ARM64_BARRIER_ISH)
#define CmiMemoryWriteFence()                __dmb(_ARM64_BARRIER_ISH)
#else
#define CmiMemoryReadFence()                 __sync_synchronize()
#define CmiMemoryWriteFence()                __sync_synchronize()
#endif

#ifdef __cplusplus
}
#endif

/* C++-only: templates cannot have C linkage */
#ifdef __cplusplus
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
#if CMI_MSVC
    static_assert(sizeof(T) == 4 || sizeof(T) == 8,
                  "CmiMemoryAtomicFetchAndInc needs a 4- or 8-byte integer");
    if constexpr (sizeof(T) == 4)
      return (T)_InterlockedExchangeAdd((volatile long *)&input, 1);
    else
      return (T)_InterlockedExchangeAdd64((volatile long long *)&input, 1);
#else
    return __atomic_fetch_add(&input, T(1), __ATOMIC_SEQ_CST);
#endif
}

#define CmiMemoryAtomicFetchAndInc(input, output) ((output) = CmiAtomicFetchAndIncImpl(input))
#elif CMI_MSVC
#define CmiMemoryAtomicFetchAndInc(input, output) ((output) = _InterlockedExchangeAdd((volatile long *)&(input), 1))
#else
#define CmiMemoryAtomicFetchAndInc(input, output) ((output) = __sync_fetch_and_add(&(input), 1))
#endif

#ifdef __cplusplus
extern "C" {
#endif

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


//partitions

/* Charm++ and NAMD both gate their replica support on this macro: with it
   undefined, +partitions/+replicas is never parsed and every replica primitive
   (replicaSend, replicaRecv, replicaBarrier, ...) compiles away to a no-op, so
   a multi-replica run silently behaves as a single one. Reconverse implements
   partitions, so turn them on. */
#define CMK_HAS_PARTITION 1

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

/* Charm++ above us numbers nodes within its own partition, while the
   communication backend numbers processes across the whole job, so every
   backend call has to cross that boundary. One partition is by far the common
   case, so keep that path to a single predictable branch. */
static inline int CmiNodeToGlobal(int node) {
  return _partitionInfo.numPartitions == 1
             ? node
             : node_lToGTranslate(node, _partitionInfo.myPartition);
}
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

#define CMI_IPC_CUTOFF_ARG "ipccutoff"
#define CMI_IPC_CUTOFF_DESC "max message size for cmi-shmem (in bytes)"
#define CMI_IPC_POOL_SIZE_ARG "ipcpoolsize"
#define CMI_IPC_POOL_SIZE_DESC "size of cmi-shmem pool (in bytes)"

/* Charm++'s charmrun forwards the ipc settings to the compute nodes through
   these environment variables, so they have to be part of the converse.h
   contract even though reconverse itself reads the ++ arguments above. */
#define CMI_IPC_CUTOFF_ENV_VAR "CmiIpcCutoff"
#define CMI_IPC_POOL_SIZE_ENV_VAR "CmiIpcPoolSize"

struct CmiIpcManager;

#ifndef __cplusplus
typedef struct CmiIpcManager CmiIpcManager;
#endif

#ifdef __cplusplus
}
#endif

/* C++-only shared-memory (IPC) API: classes, std::pair, overloads */
#ifdef __cplusplus
namespace cmi {
namespace ipc {
// recommended cutoff for block sizes
CpvExtern(size_t, kRecommendedCutoff);
// used to represent an empty linked list
constexpr auto nil = uintptr_t(0);
// used to represent the tail of a linked list
constexpr auto max = UINTPTR_MAX;
// used to indicate a message bound for a node
constexpr auto nodeDatagram = UINT16_MAX;
// default number of attempts to alloc before timing out
constexpr auto defaultTimeout = 4;
}  // namespace ipc
}  // namespace cmi

// alignas is used for padding here, rather than for alignment of the
// CmiIpcBlock itself.
struct alignas(ALIGN_BYTES) CmiIpcBlock {
public:
  // "home" rank of the block
  int src;
  int dst;
  uintptr_t orig;
  uintptr_t next;
  size_t size;

  CmiIpcBlock(size_t size_, uintptr_t orig_)
      : orig(orig_), next(cmi::ipc::nil), size(size_) {}
};

enum CmiIpcAllocStatus {
  CMI_IPC_OUT_OF_MEMORY,
  CMI_IPC_REMOTE_DESTINATION,
  CMI_IPC_SUCCESS,
  CMI_IPC_TIMEOUT
};

// sets up ipc environment
void CmiIpcInit(char** argv);

// creates an ipc manager, waking the thread when it's done
// ( this must be called in the same order on all pes! )
CmiIpcManager* CmiMakeIpcManager(CthThread th);

// push/pop blocks from the manager's send/recv queue
bool CmiPushIpcBlock(CmiIpcManager*, CmiIpcBlock*);
CmiIpcBlock* CmiPopIpcBlock(CmiIpcManager*);

// tries to allocate a block, returning null if unsucessful
// (fails when other PEs are contending resources)
// second value of pair indicates failure cause
std::pair<CmiIpcBlock*, CmiIpcAllocStatus> CmiAllocIpcBlock(CmiIpcManager*, int node, std::size_t size);

// frees a block -- enabling it to be used again
void CmiFreeIpcBlock(CmiIpcManager*, CmiIpcBlock*);

// currently a no-op but may be eventually usable
// intended to "capture" blocks from remote pes
inline void CmiCacheIpcBlock(CmiIpcBlock*) { return; }

// identifies whether a void* is the payload of a block
// belonging to the given node
CmiIpcBlock* CmiIsIpcBlock(CmiIpcManager*, void*, int node);

// if (init) is true -- initializes the
// memory segment for use as a message
void* CmiIpcBlockToMsg(CmiIpcBlock*, bool init);

// equivalent to calling above with (init = false)
inline void* CmiIpcBlockToMsg(CmiIpcBlock* block) {
  auto res = (char*)block + sizeof(CmiIpcBlock) + sizeof(CmiChunkHeader);
  return (void*)res;
}

inline CmiIpcBlock* CmiMsgToIpcBlock(CmiIpcManager* manager, void* msg) {
  return CmiIsIpcBlock(manager, (char*)msg - sizeof(CmiChunkHeader), CmiMyNode());
}

CmiIpcBlock* CmiMsgToIpcBlock(CmiIpcManager*, char* msg, std::size_t len, int node,
                           int rank = cmi::ipc::nodeDatagram,
                           int timeout = cmi::ipc::defaultTimeout);

// deliver a block as a message
void CmiDeliverIpcBlockMsg(CmiIpcBlock*);

inline const std::size_t& CmiRecommendedIpcBlockCutoff(void) {
  using namespace cmi::ipc;
  return CpvAccess(kRecommendedCutoff);
}

// Hand a message to a peer process on this host through the shared-memory
// pool, instead of over the network backend. On success the pool owns the
// message, exactly as CmiSyncSendAndFree would, and this returns true; on
// failure the message is left untouched for the caller to send some other
// way. destRank is the destination's rank within its process, or
// cmi::ipc::nodeDatagram for a message bound for that process's node queue.
//
// Fails, rather than aborting, whenever the pool cannot carry the message:
// IPC is off, the destination is this process or is on another host, the
// message is over CmiRecommendedIpcBlockCutoff(), or the pool is full.
bool CmiIpcTrySendAndFree(int destNode, int destRank, int messageSize,
                          void *msg);

// Whether the shared-memory pool is up and destNode is another process on
// this host, i.e. whether a small enough message to destNode would go through
// the pool rather than the network. A layer that follows a one-sided put with
// a notification has to know this: the put still crosses the network, but the
// notification would not, and could arrive before the data does.
bool CmiIpcReaches(int destNode);
#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

CsvExtern(CmiIpcManager*, coreIpcManager_);

/* Whether this run moves messages between processes that share a host
   through shared memory. Off unless the program was given +ipc. */
CLINKAGE int CmiIpcEnabled(void);
/* The mechanism backing the pool: "posixshm", "xpmem", or "none". */
CLINKAGE const char *CmiIpcImplName(void);
/* Processes sharing this host, this one included (1 when IPC is off). */
CLINKAGE int CmiIpcNumPeers(void);
/* Messages this PE has put into, and taken out of, the pool. Intended for
   tests and diagnostics: a PE takes messages out of the pool on behalf of
   its whole process, so the two do not balance per PE. */
CLINKAGE long CmiIpcMessagesSent(void);
CLINKAGE long CmiIpcMessagesReceived(void);

/* Keep this PE's sends off the shared-memory pool until the matching
   CmiIpcEndNetworkOnly(); nestable.

   A layer that follows a one-sided put with a notification needs this. The
   put crosses the network, but a notification handed to a peer process on
   this host would go through the pool, which does not wait for the network:
   it can arrive before the data has landed, and the receiver's callback would
   run on a buffer that is still stale. Sending the notification over the
   backend instead leaves it behind the put's local completion, which is the
   ordering the network path has always relied on.

   Only sends the pool would otherwise have carried are affected: a send to
   this process, to another host, or over CmiRecommendedIpcBlockCutoff() takes
   the same path either way.

   The depth is per-PE, so the two calls have to bracket a stretch of work on
   one PE that does not suspend: park a thread in between and every send that
   PE makes meanwhile stays off the pool too. */
CLINKAGE void CmiIpcBeginNetworkOnly(void);
CLINKAGE void CmiIpcEndNetworkOnly(void);

#ifdef __cplusplus
}
#endif

/* Persistent communication */
#include "persistent.h"

/* Task Queue Macros */
#include "taskqueue.h"
#ifdef __cplusplus
extern "C" {
#endif
CpvExtern(TaskQueue, CsdTaskQueue);

/* Steal a task from another PE on the same node */
void StealTask(void);

/* Initialize task queue work-stealing callbacks */
void CmiTaskQueueInit(void);

/* Check if the local task queue has work available */
int TaskQueueHasWork(void);

/* Pop a task from the local task queue */
void* TaskQueuePopLocal(void);

/* Push a task to the local task queue */
void TaskQueuePushLocal(void *task);

/* Process all tasks in the local queue and steal if idle */
int ProcessLocalTasks(void);

#ifdef __cplusplus
}
#endif
#define CsdTaskEnqueue(x) TaskQueuePushLocal((void*)(x))
#define CsdTaskPop() TaskQueuePopLocal()

#endif // CONVERSE_H
