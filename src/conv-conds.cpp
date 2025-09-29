#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "converse.h"

#include <deque>
#include <queue>
#include <vector>

/**
 * Structure to hold the requisites for a conditional callback
 */
struct ccd_cond_callback {
  CcdCondFn fn;
  void *arg;

  ccd_cond_callback(CcdCondFn f, void *a) : fn{f}, arg{a} {}
};

/**
 * Structure to hold the requisites for a periodic callback
 */
struct ccd_periodic_callback {
  CcdVoidFn fn;
  void *arg;
  int pe; /* the pe that sets the callback */

  ccd_periodic_callback(CcdVoidFn f, void *a, int p) : fn{f}, arg{a}, pe{p} {}
};

/**
 * A list of cond callbacks
 */
struct ccd_cblist {
  std::deque<ccd_cond_callback> elems{};
  bool flag = false;
};

/*Make sure this matches the CcdPERIODIC_* list in converse.h*/
#define CCD_PERIODIC_MAX (CcdPERIODIC_LAST - CcdPERIODIC_FIRST)

static constexpr double periodicCallInterval[CCD_PERIODIC_MAX] = {
    0.001,    0.010,    0.100,     1.0,    5.0,         10.0,       60.0,
    2 * 60.0, 5 * 60.0, 10 * 60.0, 3600.0, 12 * 3600.0, 24 * 3600.0};

/** The number of timer-based conditional callbacks */
thread_local int _ccd_num_timed_cond_cbs;

/* Cond callbacks that use the above time intervals for their condition are
 * considered "timed" */
static bool isTimed(int condnum) {
  return (condnum >= CcdPERIODIC_FIRST && condnum < CcdPERIODIC_LAST);
}

/** Remove element referred to by given list index idx. */
static inline void remove_elem(ccd_cblist &l, int condnum, int idx) {
  if (isTimed(condnum))
    _ccd_num_timed_cond_cbs--;

  l.elems.erase(l.elems.begin() + idx);
}

/** Remove n elements from the beginning of the list. */
static inline void remove_n_elems(ccd_cblist &l, int condnum, size_t n) {
  if (n == 0 || l.elems.size() < n)
    return;

  if (isTimed(condnum))
    _ccd_num_timed_cond_cbs -= n;

  l.elems.erase(l.elems.begin(), l.elems.begin() + n);
}

/** Append callback to the given cblist, and return the index. */
static inline int append_elem(ccd_cblist &l, int condnum, CcdCondFn fn,
                              void *arg) {
  if (isTimed(condnum))
    _ccd_num_timed_cond_cbs++;

  l.elems.emplace_back(fn, arg);
  return l.elems.size() - 1;
}

/**
 * Trigger the callbacks in the provided callback list and *retain* them
 * after they are called.
 *
 * Callbacks that are added after this function is started (e.g. callbacks
 * registered from other callbacks) are ignored.
 * @note: It is illegal to cancel callbacks from within ccd callbacks.
 */
static void call_cblist_keep(const ccd_cblist &l) {
  // save the length in case callbacks are added during execution
  const size_t len = l.elems.size();

  // we must iterate this way because insertion invalidates deque iterators
  for (size_t i = 0; i < len; ++i) {
    const ccd_cond_callback &cb = l.elems[i];
    (*(cb.fn))(cb.arg);
  }
}

/**
 * Trigger the callbacks in the provided callback list and *remove* them
 * from the list after they are called.
 *
 * Callbacks that are added after this function is started (e.g. callbacks
 * registered from other callbacks) are ignored.
 * @note: It is illegal to cancel callbacks from within ccd callbacks.
 */
static void call_cblist_remove(ccd_cblist &l, int condnum) {
  // save the length in case callbacks are added during execution
  const size_t len = l.elems.size();

  /* reentrant */
  if (len == 0 || l.flag)
    return;
  l.flag = true;

  // we must iterate this way because insertion invalidates deque iterators
  // i < len is correct. after i==0, unsigned underflow will wrap to SIZE_MAX
  for (size_t i = len - 1; i < len; --i) {
    const ccd_cond_callback &cb = l.elems[i];
    (*(cb.fn))(cb.arg);
  }

  remove_n_elems(l, condnum, len);
  l.flag = false;
}

#define CBLIST_INIT_LEN 8
#define MAXNUMCONDS (CcdUSERMAX + 1)

/**
 * Lists of conditional callbacks that are maintained by the scheduler
 */
struct ccd_cond_callbacks {
  ccd_cblist condcb[MAXNUMCONDS];
  ccd_cblist condcb_keep[MAXNUMCONDS];
};

/***/
thread_local static struct ccd_cond_callbacks conds;

/**
 * List of periodic callbacks maintained by the scheduler
 */
struct ccd_periodic_callbacks {
  double lastCheck; /*Time of last check*/
  double nextCall[CCD_PERIODIC_MAX];
};

/** */
thread_local static struct ccd_periodic_callbacks pcb;
thread_local int _ccd_numchecks;

/**
 * Structure used to manage periodic callbacks in a heap
 */
struct ccd_heap_elem {
  double time;
  ccd_periodic_callback cb;

  ccd_heap_elem(double t, CcdVoidFn fn, void *arg, int pe)
      : time{t}, cb{fn, arg, pe} {}

  bool operator>(const ccd_heap_elem &rhs) const {
    return this->time > rhs.time;
  }
};

/** periodic callbacks */
using ccd_heap_type =
    std::priority_queue<ccd_heap_elem, std::vector<ccd_heap_elem>,
                        std::greater<ccd_heap_elem>>;
thread_local static ccd_heap_type ccd_heap;

/**
 * How many CBs are timer-based? The scheduler can call this to check
 * if it needs to call CcdCallBacks or not.
 */
int CcdNumTimerCBs(void) { return ccd_heap.size() + _ccd_num_timed_cond_cbs; }

/**
 * Insert a new callback into the heap
 */
static inline void ccd_heap_insert(double t, CcdVoidFn fnp, void *arg,
                                   int pe = CcdIGNOREPE) {
  auto &h = ccd_heap;
  h.emplace(t, fnp, arg, pe);
}

/**
 * Identify any (over)due callbacks that were scheduled
 * and trigger them.
 */
static void ccd_heap_update(double curWallTime) {
  auto &h = ccd_heap;
  // Execute all expired heap entries
  while (!h.empty()) {
    const ccd_heap_elem &e = h.top();
    if (e.time >= curWallTime)
      break;
    const ccd_periodic_callback cb = std::move(e.cb);
    h.pop();
    (*(cb.fn))(cb.arg, curWallTime);
  }
}

void CcdCallBacksReset(void *ignored);

/**
 * Initialize the callback containers
 */
void CcdModuleInit() {
  _ccd_num_timed_cond_cbs = 0;
  _ccd_numchecks = 1;
  double curTime = CmiWallTimer();
  pcb.lastCheck = curTime;
  for (int i = 0; i < CCD_PERIODIC_MAX; i++)
    pcb.nextCall[i] = curTime + periodicCallInterval[i];
  CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, CcdCallBacksReset, 0);
  CcdCallOnConditionKeep(CcdPROCESSOR_END_IDLE, CcdCallBacksReset, 0);
}

/**
 * Register a callback function that will be triggered when the specified
 * condition is raised the next time
 */
int CcdCallOnCondition(int condnum, CcdCondFn fnp, void *arg) {
  return append_elem(conds.condcb[condnum], condnum, fnp, arg);
}

/**
 * Register a callback function that will be triggered *whenever* the specified
 * condition is raised
 */
int CcdCallOnConditionKeep(int condnum, CcdCondFn fnp, void *arg) {
  return append_elem(conds.condcb_keep[condnum], condnum, fnp, arg);
}

/**
 * Cancel a previously registered conditional callback
 */
void CcdCancelCallOnCondition(int condnum, int idx) {
  remove_elem(conds.condcb[condnum], condnum, idx);
}

/**
 * Cancel a previously registered conditional callback
 */
void CcdCancelCallOnConditionKeep(int condnum, int idx) {
  remove_elem(conds.condcb_keep[condnum], condnum, idx);
}

/**
 * Register a callback function that will be triggered on the specified PE
 * after a minimum delay of deltaT
 */
void CcdCallFnAfterOnPE(CcdVoidFn fnp, void *arg, double deltaT, int pe) {
  double ctime = CmiWallTimer();
  double tcall = ctime + deltaT * (1.0 / 1000.0);
  ccd_heap_insert(tcall, fnp, arg, pe);
}

/**
 * Register a callback function that will be triggered after a minimum
 * delay of deltaT
 */
void CcdCallFnAfter(CcdVoidFn fnp, void *arg, double deltaT) {
  double ctime = CmiWallTimer();
  double tcall = ctime + deltaT * (1.0 / 1000.0);
  ccd_heap_insert(tcall, fnp, arg);
}

/**
 * Raise a condition causing all registered callbacks corresponding to
 * that condition to be triggered
 */
void CcdRaiseCondition(int condnum) {
  call_cblist_remove(conds.condcb[condnum], condnum);
  call_cblist_keep(conds.condcb_keep[condnum]);
}

/*
 * Trigger callbacks periodically, and also the time-indexed
 * functions if their time has arrived
 */
void CcdCallBacks(void) {
  int i;
  ccd_periodic_callbacks *o = &pcb;

  /* Figure out how many times to skip Ccd processing */
  double curWallTime = CmiWallTimer();
  o->lastCheck = curWallTime;

  ccd_heap_update(curWallTime);

  for (i = 0; i < CCD_PERIODIC_MAX; i++)
    if (o->nextCall[i] <= curWallTime) {
      CcdRaiseCondition(CcdPERIODIC_FIRST + i);
      o->nextCall[i] = curWallTime + periodicCallInterval[i];
    } else
      break; /*<- because intervals are multiples of one another*/
}

/**
 * Called when something drastic changes-- restart ccd_num_checks
 */
void CcdCallBacksReset(void *ignored) {
  ccd_periodic_callbacks *o = &pcb;
  o->lastCheck = CmiWallTimer();
}
