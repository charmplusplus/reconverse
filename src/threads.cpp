#include "converse_internal.h"
#include "scheduler.h"
#include "uFcontext.h"
#include <atomic>
#include <cstring>
#include <errno.h>
#include <signal.h>

#ifndef CMK_STACKSIZE_DEFAULT
#define CMK_STACKSIZE_DEFAULT 32768
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

} CthThreadBase;

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
  CmiSetHandler(token, CpvAccess(CthResumeNormalThreadIdx));
  CmiGetQueue(CmiMyPe())->push(token);
}

void CthEnqueueSchedulingThread(CthThreadToken *token, int s, int pb,
                                unsigned int *prio) {
  CmiSetHandler(token, CpvAccess(CthResumeSchedulingThreadIdx));
  CpvStaticDeclare(int, CthResumeSchedulingThreadIdx);
  CmiGetQueue(CmiMyPe())->push(token);
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
  th->token->thread = S(th);
  th->token->serialNo = CpvAccess(Cth_serialNo)++;
  th->scheduled = 0;

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
  CpvInitialize(CthThread, doomedThreadPool);
  CpvAccess(doomedThreadPool) = (CthThread)NULL;
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
  free(th->data);

  if (th->stack != NULL) {
    free(th->stack);
  }
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
  CthFixData(t); /*Thread-local storage may have changed in other thread.*/
  CpvAccess(CthCurrent) = t;
  CpvAccess(CthData) = B(t)->data;
}

void CthResume(CthThread t) {
  CthThread tc;
  tc = CpvAccess(CthCurrent);

  if (t != tc) { /* Actually switch threads */
    CthBaseResume(t);
    if (!tc->base.exiting) {
      // CthDebug("[%d][%f] swap starts from %p to %p\n",CmiMyRank(),
      // CmiWallTimer() ,tc, t);
      if (0 != swapJcontext(&tc->context, &t->context)) {
        CmiAbort("CthResume: swapcontext failed.\n");
      }
    } else /* tc->base.exiting, so jump directly to next context */
    {
      CthThreadFree(tc);
      setJcontext(&t->context);
    }
  }

  /*This check will mistakenly fail if the thread migrates (changing tc)
    if (tc!=CthCpvAccess(CthCurrent)) { CmiAbort("Stack corrupted?\n"); }
    */
}

/*
Suspend: finds the next thread to execute, and resumes it
*/
void CthSuspend(void) {

  CthThread next;
  struct CthThreadListener *l;
  CthThreadBase *cur = B(CpvAccess(CthCurrent));

  if (cur->suspendable == 0)
    CmiAbort("Fatal Error> trying to suspend a non-suspendable thread!\n");

  CthThFn choosefn = cur->choosefn;
  if (choosefn == 0)
    CthNoStrategy();
  next = choosefn(); // If this crashes, disable ASLR.

  if (cur->scheduled > 0)
    cur->scheduled--;

  // CthDebug("[%f] next(%p) resumed\n",CmiWallTimer(), next);

  CthResume(next);
}

static void CthThreadFinished(CthThread t) {
  B(t)->exiting = 1;
  CthSuspend();
}

void CthStartThread(transfer_t arg) {
  data_t *data = (data_t *)arg.data;
  uFcontext_t *old_ucp = (uFcontext_t *)data->from;
  old_ucp->fctx = arg.fctx;
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

void CthYield(void) {
  CthAwaken(CpvAccess(CthCurrent));
  CthSuspend();
}

void CthSetNext(CthThread t, CthThread v) { B(t)->next = v; }
CthThread CthGetNext(CthThread t) { return B(t)->next; }

void CthStandinCode(void *arg) { CsdScheduler(); }

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

  if (t == NULL) {
    free(token);
    return;
  }
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
