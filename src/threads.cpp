#include <signal.h>
#include <errno.h>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "uFcontext.h"
#include "converse_internal.h"
#include "scheduler.h"
#include "uFcontext.h"
#include <atomic>
#include <cstring>
#include <errno.h>
#include <signal.h>

/* Default user-level thread stack size. Converse used a 32K fallback but
 * overrode it per-machine-layer (64K for MPI, 256K on Darwin); reconverse has
 * no such per-layer headers, so the fallback is the value everyone gets. 32K is
 * not enough for a threaded entry method that calls into a vendor runtime --
 * a single CUDA driver call (cuMemAlloc, cuLaunchKernel) can overrun it, and
 * because stacks are plain malloc'd blocks with no guard page the overflow
 * silently corrupts the neighbouring heap chunk. Override with +stacksize. */
#ifndef CMK_STACKSIZE_DEFAULT
#define CMK_STACKSIZE_DEFAULT 262144
#endif

#define THD_MAGIC_NUM 0x12345678

/*Macros to convert between base and specific thread types*/
#define B(t) ((CthThreadBase *)(t))
#define S(t) ((CthThread)(t))

typedef struct CthThreadBase {
  CthThreadToken *token; /* token that shall be enqueued into the ready queue*/
  std::atomic<int>
      scheduled; /* has this thread been added to the ready queue ? */

  CmiObjId tid;      /* globally unique tid */
  CthAwkFn awakenfn; /* Insert this thread into the ready queue */
  CthThFn choosefn;  /* Return the next ready thread */
  CthThread next;    /* Next active thread */
  int suspendable;   /* Can this thread be blocked */
  int exiting;       /* Is this thread finished */

  char *data;      /* thread private data */
  size_t datasize; /* size of thread-private data, in bytes */

  void *stack;   /*Pointer to thread stack*/
  int stacksize; /*Size of thread stack (bytes)*/
  int magic;     /* magic number for checking corruption */
  struct CthThreadListener *listener;

  int eventID;
  int srcPE;

  /* pool-style scheduling hooks (see converse.h) */
  CthAwakenArgFn awakenArgFn; /* custom awaken; NULL = default strategy */
  void *awakenArg;
  std::atomic<int> state;     /* CTH_STATE_* */
  int isPeMain;               /* the PE's original OS-thread context */
  int homeRank;               /* rank whose queue a pinned main thread is pushed to */
  int keepOnExit;             /* 1: do not free at exit (CthFree later) */
  CthVoidFn exitFn;           /* run post-switch when the thread exits */
  void *exitArg;
  void *userData;
} CthThreadBase;

/* Action deferred until after the next context switch on this PE. It is
 * recorded by the suspending thread and executed on the resumed thread's
 * stack, once the suspending thread is off its stack. */
enum {
  CTH_POST_NONE = 0,
  CTH_POST_THEN,      /* fn(arg) */
  CTH_POST_BLOCK,     /* thread->state = BLOCKED; fn(arg) */
  CTH_POST_YIELD,     /* thread->state = READY; awaken(thread) */
  CTH_POST_TERMINATE  /* if keep: thread->state = TERMINATED; fn(arg) */
};
struct CthPostSwitch {
  int kind;
  CthThread thread;
  CthVoidFn fn;
  void *arg;
  int keep;
};

struct CthThreadStruct {
  CthThreadBase base;
  double *dummy;
  uFcontext_t context;
};

CpvStaticDeclare(CthThread, CthCurrent); /*Current thread*/
CpvDeclare(char *,
           CthData); /*Current thread's private data (externally visible)*/
CpvStaticDeclare(size_t, CthDatasize);
/* Threads waiting to be destroyed */
CpvStaticDeclare(CthThread, doomedThreadPool);
CpvStaticDeclare(int, Cth_serialNo);
CpvStaticDeclare(int, _defaultStackSize);
CpvDeclare(int, CthResumeNormalThreadIdx);
CpvStaticDeclare(int, CthResumeSchedulingThreadIdx);

// main and scheduling threads
CpvStaticDeclare(CthThread, CthMainThread);
CpvStaticDeclare(CthThread, CthSchedulingThread);
CpvStaticDeclare(CthThread, CthSleepingStandins);
CpvStaticDeclare(CthPostSwitch, CthPost);

/* Handler indices are identical on every PE (same registration order), so a
 * process-wide copy lets a non-PE thread (no Cpv storage) awaken a thread. */
static int CthResumeNormalIdxGlobal = -1;
static int CthResumeSchedulingIdxGlobal = -1;

/* debug ring of thread events per PE, dumped by CthSetPost on conflict */
struct CthTraceEv { int ev; const void *a; const void *b; int x; };
CpvStaticDeclare(CthTraceEv *, CthTrace);
CpvStaticDeclare(int, CthTraceIdx);
#define CTH_TRACE_N 64
static inline void CthTr(int ev, const void *a, const void *b, int x) {
  CthTraceEv *r = CpvAccess(CthTrace);
  if (!r) return;
  int i = CpvAccess(CthTraceIdx)++ & (CTH_TRACE_N - 1);
  r[i] = CthTraceEv{ev, a, b, x};
}
static void CthTraceDump(void) {
  CthTraceEv *r = CpvAccess(CthTrace);
  if (!r) return;
  static const char *names[] = {"-", "setpost", "drain", "resume", "suspend", "finished", "resumeNormal", "resumeSched", "startThread"};
  int n = CpvAccess(CthTraceIdx);
  fprintf(stderr, "[%d] Cth trace (oldest first):\n", CmiMyPe());
  for (int k = n - CTH_TRACE_N; k < n; k++) {
    if (k < 0) continue;
    CthTraceEv &e = r[k & (CTH_TRACE_N - 1)];
    fprintf(stderr, "[%d]   %-12s a=%p b=%p x=%d\n", CmiMyPe(), names[e.ev], e.a, e.b, e.x);
  }
}

static void CthRunPostSwitch(void) {
  CthPostSwitch p = CpvAccess(CthPost);
  if (p.kind == CTH_POST_NONE) return;
  CpvAccess(CthPost).kind = CTH_POST_NONE;
  CthTr(2, (void *)p.thread, (void *)CpvAccess(CthCurrent), p.kind);
  switch (p.kind) {
  case CTH_POST_THEN:
    if (p.fn) p.fn(p.arg);
    break;
  case CTH_POST_BLOCK:
    B(p.thread)->state.store(CTH_STATE_BLOCKED, std::memory_order_release);
    if (p.fn) p.fn(p.arg);
    break;
  case CTH_POST_YIELD: {
    CthThreadBase *th = B(p.thread);
    th->state.store(CTH_STATE_READY, std::memory_order_release);
    th->awakenfn(th->token, CQS_QUEUEING_FIFO, 0, 0);
    break;
  }
  case CTH_POST_TERMINATE:
    if (p.keep)
      B(p.thread)->state.store(CTH_STATE_TERMINATED, std::memory_order_release);
    if (p.fn) p.fn(p.arg);
    break;
  }
}

static void CthSetPost(int kind, CthThread t, CthVoidFn fn, void *arg, int keep) {
  CthPostSwitch &p = CpvAccess(CthPost);
  CthTr(1, (void *)t, (void *)CpvAccess(CthCurrent), kind);
  if (p.kind != CTH_POST_NONE) {
    CthTraceDump();
    CmiAbort("Cth: a post-switch action (kind %d, thread %p exiting=%d state=%d) is already pending on PE %d "
             "while setting kind %d for thread %p (current %p, sched %p, main %p, standins %p)\n",
             p.kind, (void *)p.thread, B(p.thread)->exiting, B(p.thread)->state.load(), CmiMyPe(), kind, (void *)t,
             (void *)CpvAccess(CthCurrent), (void *)CpvAccess(CthSchedulingThread), (void *)CpvAccess(CthMainThread),
             (void *)CpvAccess(CthSleepingStandins));
  }
  p.kind = kind; p.thread = t; p.fn = fn; p.arg = arg; p.keep = keep;
}

CthThreadToken *CthGetToken(CthThread t) { return B(t)->token; }

static void CthFixData(CthThread t) {
  size_t newsize = CpvAccess(CthDatasize);
  size_t oldsize = B(t)->datasize;
  if (oldsize < newsize) {
    newsize = 2 * newsize;
    B(t)->datasize = newsize;
    /* Note: realloc(NULL,size) is equivalent to malloc(size) */
    B(t)->data = (char *)realloc(B(t)->data, newsize);
    memset(B(t)->data + oldsize, 0, newsize - oldsize);
  }
}

void CthSetThreadID(CthThread th, int a, int b, int c) {
  B(th)->tid.id[0] = a;
  B(th)->tid.id[1] = b;
  B(th)->tid.id[2] = c;
}

CmiObjId *CthGetThreadID(CthThread th) { return &(B(th)->tid); }

static void CthNoStrategy(void) {
  CmiAbort("Called CthAwaken or CthSuspend before calling CthSetStrategy.\n");
}

void CthSetStrategy(CthThread t, CthAwkFn awkfn, CthThFn chsfn) {
  B(t)->awakenfn = awkfn;
  B(t)->choosefn = chsfn;
}

void CthEnqueueNormalThread(CthThreadToken *token, int s, int pb,
                            unsigned int *prio) {
  if (Cmi_myrank < 0)
    CmiAbort("CthAwaken from a non-PE thread: the default strategy pushes to "
             "the caller's self queue, which no PE polls. Give the thread a "
             "custom awaken function (CthSetAwakenFn).\n");
  CmiSetHandler(token, CpvAccess(CthResumeNormalThreadIdx));
  // the token always goes to the PE that is awakening the thread, so it can
  // take the self queue and skip the shared queue's atomics
  CmiGetSelfQueue()->push(token);
}

void CthEnqueueSchedulingThread(CthThreadToken *token, int s, int pb,
                                unsigned int *prio) {
  CmiSetHandler(token, CpvAccess(CthResumeSchedulingThreadIdx));
  CmiGetSelfQueue()->push(token);
}

/* Custom-awaken trampoline: the token carries the resume flavour, then goes
 * wherever fn(t, arg) puts it. A PE main thread is pinned to its own PE:
 * its scheduling-resume token must be popped by that PE (the standin
 * bookkeeping in CthResumeSchedulingThread is per-PE), so it goes to the
 * home PE's queue instead of the custom destination. Usable from a non-PE
 * thread: no Cpv access on this path. */
static void CthEnqueueCustomThread(CthThreadToken *token, int s, int pb,
                                   unsigned int *prio) {
  CthThreadBase *th = B(token->thread);
  if (th->isPeMain) {
    /* A PE main thread carries the scheduling-resume flavour and MUST be
     * popped by its own PE (CthResumeSchedulingThread's bookkeeping is
     * per-PE). The custom function is still called so the token can take
     * its place in the caller's queue order; the contract is that the
     * queue only lets PE homeRank resume it. Without a custom function the
     * token goes straight to the home PE's queue. */
    CmiSetHandler(token, CthResumeSchedulingIdxGlobal);
    if (th->awakenArgFn) th->awakenArgFn(token->thread, th->awakenArg);
    else CmiPushPE(th->homeRank, (int)sizeof(CthThreadToken), token);
  } else {
    CmiSetHandler(token, CthResumeNormalIdxGlobal);
    th->awakenArgFn(token->thread, th->awakenArg);
  }
}

static CthThread CthSuspendNormalThread(void) {
  return CpvAccess(CthSchedulingThread);
}

void CthSetStrategyDefault(CthThread t) {
  CthSetStrategy(t, CthEnqueueNormalThread, CthSuspendNormalThread);
}

static void CthBaseInit(char **argv) {
  char *str;

  CpvInitialize(int, _defaultStackSize);
  CpvAccess(_defaultStackSize) = CMK_STACKSIZE_DEFAULT;
  /* CthInit is public API and may legitimately be handed a null argv. */
  if (argv != NULL && CmiGetArgStringDesc(argv, "+stacksize", &str,
                                          "Default user-level thread stack size")) {
    CpvAccess(_defaultStackSize) = (int)CmiReadSize(str);
  }

  CpvInitialize(CthThread, CthCurrent);
  CpvInitialize(char *, CthData);
  CpvInitialize(size_t, CthDatasize);

  CpvAccess(CthData) = 0;
  CpvAccess(CthDatasize) = 0;

  CpvInitialize(int, Cth_serialNo);
  CpvAccess(Cth_serialNo) = 1;
}

CthThread CthSelf(void) { return CpvAccess(CthCurrent); }

static void CthThreadInit(CthThread t) {
  CthThreadBase *th = &t->base;

  static std::atomic<int> serialno{1};
  th->token = (CthThreadToken *)malloc(sizeof(CthThreadToken));
  memset(th->token, 0, sizeof(CthThreadToken));
  CmiInitMsgHeader(th->token, (int)sizeof(CthThreadToken));
  th->token->thread = S(th);
  th->token->serialNo = CpvAccess(Cth_serialNo)++;
  th->scheduled = 0;

  th->awakenArgFn = NULL;
  th->awakenArg = NULL;
  th->state.store(CTH_STATE_BLOCKED); /* runnable only once awakened */
  th->isPeMain = 0;
  th->homeRank = -1;
  th->keepOnExit = 0;
  th->exitFn = NULL;
  th->exitArg = NULL;
  th->userData = NULL;
  th->eventID = 0;
  th->srcPE = -1;

  th->awakenfn = 0;
  th->choosefn = 0;
  th->next = 0;
  th->suspendable = 1;
  th->exiting = 0;

  th->data = 0;
  th->datasize = 0;
  CthFixData(S(th));

  CthSetStrategyDefault(S(th));

  th->stack = NULL;
  th->stacksize = 0;

  th->tid.id[0] = CmiMyPe();
  th->tid.id[1] = std::atomic_fetch_add(&serialno, 1);
  th->tid.id[2] = 0;

  th->listener = NULL;

  th->magic = THD_MAGIC_NUM;
}

void CthInit(char **argv) {
  CthThread t;

  CthBaseInit(argv);
  t = (CthThread)malloc(sizeof(struct CthThreadStruct));
  //_MEMCHECK(t);
  CpvAccess(CthCurrent) = t;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (0 != getJcontext(&t->context))
    CmiAbort("CthInit: getcontext failed.\n");
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
  CthThreadInit(t);
  t->base.isPeMain = 1;
  t->base.homeRank = CmiMyRank();
  t->base.state.store(CTH_STATE_RUNNING);
  CpvInitialize(CthThread, doomedThreadPool);
  CpvAccess(doomedThreadPool) = (CthThread)NULL;
  CpvInitialize(CthPostSwitch, CthPost);
  CpvAccess(CthPost).kind = CTH_POST_NONE;
  CpvInitialize(CthTraceEv *, CthTrace);
  CpvInitialize(int, CthTraceIdx);
  CpvAccess(CthTraceIdx) = 0;
  CpvAccess(CthTrace) = getenv("CTH_TRACE") ? (CthTraceEv *)calloc(CTH_TRACE_N, sizeof(CthTraceEv)) : nullptr;
}

static void *CthAllocateStack(CthThreadBase *th, int *stackSize,
                              int useMigratable) {
  void *ret = NULL;
  if (*stackSize == 0)
    *stackSize = CpvAccess(_defaultStackSize);
  th->stacksize = *stackSize;

  ret = malloc(*stackSize);
  // CmiEnforce(ret != nullptr);

  th->stack = ret;

  return ret;
}

static CthThread CthCreateInner(CthVoidFn fn, void *arg, int size,
                                int migratable) {
  CthThread result;
  char *stack, *ss_sp, *ss_end;

  result = (CthThread)malloc(sizeof(struct CthThreadStruct));
  //_MEMCHECK(result);
  CthThreadInit(result);
  CthAllocateStack(&result->base, &size, migratable);
  stack = (char *)result->base.stack;
  ss_end = stack + size;

  /**
    Decide where to point the uc_stack.ss_sp field of our "context"
    structure.  The configuration values CMK_CONTEXT_STACKBEGIN,
    CMK_CONTEXT_STACKEND, and CMK_CONTEXT_STACKMIDDLE determine where to
    point ss_sp: to the beginning, end, and middle of the stack buffer
    respectively.  The default, used by most machines, is
    CMK_CONTEXT_STACKBEGIN.
    */

  ss_sp = (char *)stack + size;
  ss_end = stack;

  result->context.uc_stack.ss_sp = ss_sp;
  result->context.uc_stack.ss_size = size;

  result->context.uc_stack.ss_flags = 0;
  result->context.uc_link = 0;

  errno = 0;
  makeJcontext(&result->context, (uFcontext_fn_t)CthStartThread, fn, arg);

  if (errno != 0) {
    perror("makecontext");
    CmiAbort("CthCreateInner: makecontext failed.\n");
  }

  return result;
}

CthThread CthCreate(CthVoidFn fn, void *arg, int size) {
  return CthCreateInner(fn, arg, size, 0);
}

static void CthThreadBaseFree(CthThreadBase *th) {
  struct CthThreadListener *l, *lnext;
  /*
   * remove the token if it is not queued in the converse scheduler
   */
  if (th->scheduled == 0) {
    free(th->token);
  } else {
    th->token->thread = NULL;
  }
  /* Call the free function pointer on all the listeners on
     this thread and also delete the thread listener objects
     */
  for (l = th->listener; l != NULL; l = lnext) {
    lnext = l->next;
    l->next = 0;
    if (l->free)
      l->free(l);
  }
  th->listener = NULL;
  free(th->data);
  th->data = NULL;

  if (th->stack != NULL)
    free(th->stack);
  th->stack = NULL;
}

static void CthThreadFree(CthThread t) {
  /* avoid freeing thread while it is being used, store in pool and
     free it next time. Note the last thread in pool won't be free'd! */
  CthThread doomed = CpvAccess(doomedThreadPool);
  CpvAccess(doomedThreadPool) = t;
  if (doomed != NULL) {
    CthThreadBaseFree(&doomed->base);
    free(doomed);
  }
}

static void CthBaseResume(CthThread t) {
  struct CthThreadListener *l;
  for (l = B(t)->listener; l != NULL; l = l->next) {
    if (l->resume)
      l->resume(l);
  }
  CthFixData(t); /*Thread-local storage may have changed in other thread.*/
  CpvAccess(CthCurrent) = t;
  CpvAccess(CthData) = B(t)->data;
}

void CthResume(CthThread t) {
  CthThread tc;
  tc = CpvAccess(CthCurrent);

  if (t != tc) { /* Actually switch threads */
    CthTr(3, (void *)t, (void *)tc, tc->base.exiting);
    CthBaseResume(t);
    if (!tc->base.exiting) {
      if (0 != swapJcontext(&tc->context, &t->context)) {
        CmiAbort("CthResume: swapcontext failed.\n");
      }
      /* We are back on tc, resumed by some other thread that has just
       * switched away: run the action it deferred until it was off-stack. */
      CthRunPostSwitch();
    } else /* tc->base.exiting, so jump directly to next context */
    {
      if (!tc->base.keepOnExit)
        CthThreadFree(tc);
      setJcontext(&t->context);
    }
  }

  /*This check will mistakenly fail if the thread migrates (changing tc)
    if (tc!=CthCpvAccess(CthCurrent)) { CmiAbort("Stack corrupted?\n"); }
    */
}

int CthIsSuspendable(CthThread t) { return B(t)->suspendable; }

/*
Suspend: finds the next thread to execute, and resumes it
*/
void CthSuspend(void) {

  CthThread next;
  struct CthThreadListener *l;
  CthThreadBase *cur = B(CpvAccess(CthCurrent));

  if (cur->suspendable == 0)
    CmiAbort("Fatal Error> trying to suspend a non-suspendable thread!\n");

  /* Call the suspend function on listeners */
  for (l = cur->listener; l != NULL; l = l->next) {
    if (l->suspend)
      l->suspend(l);
  }

  CthThFn choosefn = cur->choosefn;
  if (choosefn == 0)
    CthNoStrategy();
  next = choosefn(); // If this crashes, disable ASLR.
  CthTr(4, (void *)cur, (void *)next, cur->exiting);

  if (cur->scheduled > 0)
    cur->scheduled--;

  // CthDebug("[%f] next(%p) resumed\n",CmiWallTimer(), next);

  CthResume(next);
}

static void CthThreadFinished(CthThread t) {
  CthThreadBase *th = B(t);
  if (th->keepOnExit || th->exitFn)
    /* TERMINATED and the exit callback are published only once this stack
     * is no longer in use, so a joiner woken by exitFn may free the thread. */
    CthSetPost(CTH_POST_TERMINATE, t, th->exitFn, th->exitArg, th->keepOnExit);
  th->exiting = 1;
  CthSuspend();
}

void CthStartThread(transfer_t arg) {
  data_t *data = (data_t *)arg.data;
  uFcontext_t *old_ucp = (uFcontext_t *)data->from;
  if (old_ucp) old_ucp->fctx = arg.fctx; /* NULL when entered via setJcontext */
  CthTr(8, (void *)CpvAccess(CthCurrent), old_ucp, 0);
  CthRunPostSwitch();
  uFcontext_t *cur_ucp = (uFcontext_t *)data->data;
  cur_ucp->func(cur_ucp->arg);
  CthThreadFinished(CthSelf());
}

void CthAwaken(CthThread th) {
  CthAwkFn awakenfn = B(th)->awakenfn;
  if (awakenfn == 0)
    CthNoStrategy();
  B(th)->scheduled++;
  CthThreadToken *token = B(th)->token;
  constexpr int strategy = CQS_QUEUEING_FIFO;
  awakenfn(token, strategy, 0, 0); // If this crashes, disable ASLR.
}

void CthAwakenPrio(CthThread th, int s, int pb, unsigned int *prio)
{
  CthAwkFn awakenfn = B(th)->awakenfn;
  if (awakenfn == 0) CthNoStrategy();
  /* count before enqueueing: once the token is in a queue another PE may
   * pop it and free the thread, and CthThreadBaseFree reads 'scheduled' */
  B(th)->scheduled++;
  CthThreadToken * token = B(th)->token;
  awakenfn(token, s, pb, prio); // If this crashes, disable ASLR.
}

void CthYield(void) {
  CthThread self = CpvAccess(CthCurrent);
  if (B(self)->awakenArgFn) {
    /* the token may only enter a shared queue once we are off this stack */
    CthSetPost(CTH_POST_YIELD, self, NULL, NULL, 0);
    CthSuspend();
    return;
  }
  CthAwaken(self);
  CthSuspend();
}

/* ---- pool-style scheduling hooks ---- */

void CthSetAwakenFn(CthThread t, CthAwakenArgFn fn, void *arg) {
  CthThreadBase *th = B(t);
  th->awakenArgFn = fn;
  th->awakenArg = arg;
  if (fn)
    th->awakenfn = CthEnqueueCustomThread;
  else
    th->awakenfn = th->isPeMain ? CthEnqueueSchedulingThread
                                : CthEnqueueNormalThread;
  if (t == CpvAccess(CthCurrent))
    th->state.store(CTH_STATE_RUNNING);
}

void *CthGetAwakenArg(CthThread t) { return B(t)->awakenArg; }
void CthSetChooseFn(CthThread t, CthThFn chsfn) { B(t)->choosefn = chsfn; }
CthThread CthGetSchedulingThread(void) { return CpvAccess(CthSchedulingThread); }
int CthIsPeMainThread(CthThread t) { return B(t)->isPeMain; }
int CthGetHomeRank(CthThread t) { return B(t)->homeRank; }

int CthAwakenIfBlocked(CthThread t) {
  CthThreadBase *th = B(t);
  int expected = CTH_STATE_BLOCKED;
  if (!th->state.compare_exchange_strong(expected, CTH_STATE_READY,
                                         std::memory_order_acq_rel))
    return 0;
  if (th->awakenfn == 0) CthNoStrategy();
  th->awakenfn(th->token, CQS_QUEUEING_FIFO, 0, 0);
  return 1;
}

void CthSuspendThen(CthVoidFn after, void *arg) {
  CthSetPost(CTH_POST_THEN, CpvAccess(CthCurrent), after, arg, 0);
  CthSuspend();
}

void CthSuspendBlocked(CthVoidFn after, void *arg) {
  CthSetPost(CTH_POST_BLOCK, CpvAccess(CthCurrent), after, arg, 0);
  CthSuspend();
}

int CthGetState(CthThread t) { return B(t)->state.load(std::memory_order_acquire); }
void CthSetKeepOnExit(CthThread t, int keep) { B(t)->keepOnExit = keep; }
void CthSetExitFn(CthThread t, CthVoidFn fn, void *arg) {
  B(t)->exitFn = fn;
  B(t)->exitArg = arg;
}
void CthSetUserData(CthThread t, void *data) { B(t)->userData = data; }
void *CthGetUserData(CthThread t) { return B(t)->userData; }
void CthSetSuspendable(CthThread t, int val) { B(t)->suspendable = val; }

void CthSetNext(CthThread t, CthThread v) { B(t)->next = v; }
CthThread CthGetNext(CthThread t) { return B(t)->next; }

void CthStandinCode(void *arg) {
  CsdScheduler();
  /* The scheduler was told to stop while running on a standin. Hand control
   * back to the PE's main thread, which is parked inside
   * CthResumeSchedulingThread and will return to its own scheduler loop and
   * exit normally. Falling off the end instead would re-enter
   * CthSuspendSchedulingThread and create standins forever. */
  CthThread me = CthSelf();
  CthThread mainTh = CpvAccess(CthMainThread);
  if (B(mainTh)->awakenArgFn)
    CmiAbort("CsdScheduler stopped on a standin while this PE's main thread "
             "is a user thread (custom awaken); nothing to return to.\n");
  CthSetNext(me, CpvAccess(CthSleepingStandins));
  CpvAccess(CthSleepingStandins) = me;
  CpvAccess(CthSchedulingThread) = mainTh;
  CthResume(mainTh);
}

CthThread CthSuspendSchedulingThread(void) {
  CthThread succ = CpvAccess(CthSleepingStandins);

  if (succ) {
    CpvAccess(CthSleepingStandins) = CthGetNext(succ);
  } else {
    succ = CthCreate(CthStandinCode, 0, 256000);
    CthSetStrategy(succ, CthEnqueueSchedulingThread,
                   CthSuspendSchedulingThread);
  }

  CpvAccess(CthSchedulingThread) = succ;
  return succ;
}

/* Notice: For changes to the following function, make sure the function
 * CthResumeNormalThreadDebug is also kept updated. */
void CthResumeNormalThread(CthThreadToken *token) {
  CthThread t = token->thread;
  CthTr(6, (void *)t, (void *)token, t ? B(t)->state.load() : -1);

  if (t == NULL) {
    free(token);
    return;
  }
  if (B(t)->awakenArgFn) {
    /* a custom-awaken thread's token must be popped exactly once per wake:
     * READY is the only state it can be in here */
    int st = B(t)->state.load(std::memory_order_acquire);
    if (st != CTH_STATE_READY)
      CmiAbort("CthResumeNormalThread: token of thread %p popped on PE %d in state %d "
               "(exiting=%d) -- the token was in a queue twice or the thread was still running\n",
               (void *)t, CmiMyPe(), st, B(t)->exiting);
  }
  B(t)->state.store(CTH_STATE_RUNNING, std::memory_order_relaxed);
  CthResume(t);
}

int CthIsMainThread(CthThread t) { return t == CpvAccess(CthMainThread); }

void CthResumeSchedulingThread(CthThreadToken *token) {
  CthThread t = token->thread;
  CthThread me = CthSelf();
  if (CthIsMainThread(me)) {
    CthEnqueueSchedulingThread(CthGetToken(me), CQS_QUEUEING_FIFO, 0, 0);
  } else {
    CthSetNext(me, CpvAccess(CthSleepingStandins));
    CpvAccess(CthSleepingStandins) = me;
  }
  CpvAccess(CthSchedulingThread) = t;

  B(t)->state.store(CTH_STATE_RUNNING, std::memory_order_relaxed);
  CthResume(t);
}

void CthTraceResume(CthThread t) {
  // no tracing
}

void CthAddListener(CthThread t, struct CthThreadListener *l) {
  struct CthThreadListener *p = B(t)->listener;
  if (p == NULL) { /* first listener */
    B(t)->listener = l;
    l->thread = t;
    l->next = NULL;
    return;
  }
  /* Add l at end of current chain of listeners: */
  while (p->next != NULL) {
    p = p->next;
  }
  p->next = l;
  l->next = NULL;
  l->thread = t;
}

void CthSchedInit() {
  CpvInitialize(CthThread, CthMainThread);
  CpvInitialize(CthThread, CthSchedulingThread);
  CpvInitialize(CthThread, CthSleepingStandins);
  CpvInitialize(int, CthResumeNormalThreadIdx);
  CpvInitialize(int, CthResumeSchedulingThreadIdx);

  CpvAccess(CthMainThread) = CthSelf();
  CpvAccess(CthSchedulingThread) = CthSelf();
  CpvAccess(CthSleepingStandins) = 0;
  CpvAccess(CthResumeNormalThreadIdx) =
      CmiRegisterHandler((CmiHandler)CthResumeNormalThread);
  CpvAccess(CthResumeSchedulingThreadIdx) =
      CmiRegisterHandler((CmiHandler)CthResumeSchedulingThread);
  /* same on every PE by construction (identical registration order) */
  if (CthResumeNormalIdxGlobal < 0) {
    CthResumeNormalIdxGlobal = CpvAccess(CthResumeNormalThreadIdx);
    CthResumeSchedulingIdxGlobal = CpvAccess(CthResumeSchedulingThreadIdx);
  } else if (CthResumeNormalIdxGlobal != CpvAccess(CthResumeNormalThreadIdx) ||
             CthResumeSchedulingIdxGlobal != CpvAccess(CthResumeSchedulingThreadIdx)) {
    CmiAbort("CthSchedInit: thread-resume handler indices differ across PEs\n");
  }
  CthSetStrategy(CthSelf(), CthEnqueueSchedulingThread,
                 CthSuspendSchedulingThread);
}

// helpers for Ctv variables
size_t CthRegister(size_t size) {
  size_t datasize = CthCpvAccess(CthDatasize);
  CthThreadBase *th = (CthThreadBase *)CthCpvAccess(CthCurrent);
  size_t result, align = 1;
  while (size > align)
    align <<= 1;
  datasize = (datasize + align - 1) & ~(align - 1);
  result = datasize;
  datasize += size;
  CthCpvAccess(CthDatasize) = datasize;
  CthFixData(S(th)); /*Make the current thread have this much storage*/
  CthCpvAccess(CthData) = th->data;
  return result;
}

/**
  Make sure we have room to store up to at least maxOffset
  bytes of thread-local storage.
  */
void CthRegistered(size_t maxOffset) {
  if (CthCpvAccess(CthDatasize) < maxOffset) {
    CthThreadBase *th = (CthThreadBase *)CthCpvAccess(CthCurrent);
    CthCpvAccess(CthDatasize) = maxOffset;
    CthFixData(S(th)); /*Make the current thread have this much storage*/
    CthCpvAccess(CthData) = th->data;
  }
}

/* possible hack? CW */
char *CthGetData(CthThread t) { return B(t)->data; }

void CthSetEventInfo(CthThread t, int event, int srcPE) 
{
  B(t)->eventID = event;
  B(t)->srcPE = srcPE;
}

void CthFree(CthThread t)
{
  if (t==NULL) return;

  if (t != CthSelf()) {
    CthThreadFree(t);
  } else
    t->base.exiting = 1;
}

int CthImplemented(void) { return 1; }