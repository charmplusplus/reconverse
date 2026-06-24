
#include <stdlib.h>
#include "cldb.h"
#include <math.h>

typedef char *BitVector;

CpvDeclare(int, CldHandlerIndex);

thread_local int CldNodeHandlerIndex;
thread_local int CldBalanceHandlerIndex;
thread_local int CldRelocatedMessages;
thread_local int CldLoadBalanceMessages;
thread_local int CldMessageChunks;

// ---------------------------------------------------------------------------
// Token queue: a doubly-linked ring of stealable work items.
//
// Each "token" is itself a valid Converse message (starts with a message
// header) so the scheduler can dispatch it like any other message.  When the
// scheduler dequeues a token, CldTokenHandler fires:
//   - if tok->msg is still set, the real message is executed and the token is
//     removed from the ring;
//   - if tok->msg is nullptr, another PE already stole it (via CldGetToken),
//     so we just discard the empty token.
// ---------------------------------------------------------------------------

struct CldToken_s {
  char msg_header[CmiMsgHeaderSizeBytes]; // must be first – treated as a Cmi msg
  char *msg;                              // nullptr once stolen
  CldToken_s *pred;
  CldToken_s *succ;
};
using CldToken = CldToken_s *;

struct CldProcInfo_s {
  int token_handler_idx;
  int load;        // tokens currently in the ring
  CldToken sentinel;
};
using CldProcInfo = CldProcInfo_s *;

thread_local CldProcInfo CldProc = nullptr;

static char s_lbtopo_default[] = "torus_nd_5";
extern char *_lbtopo;
char *_lbtopo = s_lbtopo_default;

extern void LoadNotifyFn(int);

/* Estimator stuff.  Of any use? */
/*
CpvStaticDeclare(CldEstimatorTable, _estfns);
*/
void CldRegisterEstimator(CldEstimator fn) {
  /*CpvAccess(_estfns).fns[CpvAccess(_estfns).count++] = fn;*/
}

/*
int CldEstimate(void)
{
  CldEstimatorTable *estab = &(CpvAccess(_estfns));
  int i, load=0;
  for(i=0; i<estab->count; i++)
    load += (*(estab->fns[i]))();
  return load;
}

static int CsdEstimator(void)
{
  return CsdLength();
}
*/

int CldRegisterInfoFn(CldInfoFn fn) {
  return CmiRegisterHandler((CmiHandler)fn);
}

int CldRegisterPackFn(CldPackFn fn) {
  return CmiRegisterHandler((CmiHandler)fn);
}

/* CldSwitchHandler takes a message and a new handler number.  It
 * changes the handler number to the new handler number and move the
 * old to the Xhandler part of the header.  When the message gets
 * handled, the handler should call CldRestoreHandler to put the old
 * handler back.
 *
 * CldPutToken puts a message in the scheduler queue in such a way
 * that it can be retreived from the queue.  Once the message gets
 * handled, it can no longer be retreived.  CldGetToken removes a
 * message that was placed in the scheduler queue in this way.
 * CldCountTokens tells you how many tokens are currently retreivable.
 */

void CldSwitchHandler(char *cmsg, int handler) {
  CmiSetXHandler(cmsg, CmiGetHandler(cmsg));
  CmiSetHandler(cmsg, handler);
}

void CldRestoreHandler(char *cmsg) {
  CmiSetHandler(cmsg, CmiGetXHandler(cmsg));
}

void CldHandler(char *);

// ---------------------------------------------------------------------------
// CldTokenHandler – dispatched by the scheduler when it dequeues a token.
// ---------------------------------------------------------------------------
static void CldTokenHandler(void *vtok) {
  CldToken tok = static_cast<CldToken>(vtok);
  CldProcInfo proc = CldProc;
  if (tok->msg) {
    // Remove token from the ring before dispatching so CldCountTokens is
    // accurate during the handler.
    tok->pred->succ = tok->succ;
    tok->succ->pred = tok->pred;
    proc->load--;
    char *msg = tok->msg;
    CmiFree(tok);
    CmiHandleMessage(msg);
  } else {
    // Token was stolen; the ring entry was already removed by CldGetToken.
    CmiFree(tok);
  }
  LoadNotifyFn(proc->load);
}

// ---------------------------------------------------------------------------
// Token queue public API
// ---------------------------------------------------------------------------

void CldPutToken(char *msg) {
  CldProcInfo proc = CldProc;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  CldToken tok = static_cast<CldToken>(CmiAlloc(sizeof(CldToken_s)));
  tok->msg = msg;

  // Append before the sentinel (end of ring).
  tok->pred = proc->sentinel->pred;
  tok->succ = proc->sentinel;
  tok->pred->succ = tok;
  tok->succ->pred = tok;
  proc->load++;

  // Register the token as a dispatchable message in the scheduler queue.
  CmiSetHandler(tok, proc->token_handler_idx);
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CsdEnqueueGeneral(tok, queueing, priobits, prioptr);
}

void CldGetToken(char **msg) {
  CldProcInfo proc = CldProc;
  CldToken tok = proc->sentinel->succ;
  if (tok == proc->sentinel) {
    *msg = nullptr;
    return;
  }
  // Remove from ring so it cannot be processed locally.
  tok->pred->succ = tok->succ;
  tok->succ->pred = tok->pred;
  proc->load--;
  *msg = tok->msg;
  tok->msg = nullptr; // CldTokenHandler will see nullptr and just free the tok
}

int CldCountTokens() {
  return CldProc ? CldProc->load : 0;
}

// ---------------------------------------------------------------------------
// CldModuleGeneralInit – called by every strategy's CldModuleInit.
// Initialises the per-PE token queue for this thread.
// ---------------------------------------------------------------------------
void CldModuleGeneralInit(char **argv) {
  CldToken sentinel = static_cast<CldToken>(CmiAlloc(sizeof(CldToken_s)));
  sentinel->succ = sentinel;
  sentinel->pred = sentinel;
  sentinel->msg  = nullptr;

  CldProc = new CldProcInfo_s();
  CldProc->sentinel = sentinel;
  CldProc->load = 0;
  CldProc->token_handler_idx = CmiRegisterHandler((CmiHandler)CldTokenHandler);
}