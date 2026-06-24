// Work-stealing seed load balancer for Reconverse.
//
// When a chare is created with CK_PE_ANY (CLD_ANYWHERE), its creation message
// is placed in the local token queue on the creating PE rather than being
// dispatched to a random destination immediately.  Idle PEs detect idleness
// via the CcdPROCESSOR_STILL_IDLE callback and send a steal request to a
// randomly chosen victim.  The victim, if it has stealable tokens, extracts
// one with CldGetToken and forwards it to the requester.  If no tokens are
// available, the victim sends back an ack-no-task reply so the requester can
// retry on the next idle callback.
//
// Handler registration order (must be identical on every PE):
//   1. CldHandler              (registered in CldModuleInit below)
//   2. CldNodeHandler
//   3. CldBalanceHandler
//   4. CldAskLoadHandler
//   5. CldAckNoTaskHandler
//   6. CldTokenHandler         (registered inside CldModuleGeneralInit)

#include "cldb.h"
#include "converse_internal.h"
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Per-PE work-stealing state (thread-local because each PE runs one thread)
// ---------------------------------------------------------------------------

thread_local int CldAskLoadHandlerIndex;
thread_local int CldAckNoTaskHandlerIndex;
thread_local bool CldIsStealing = false;

// Steal-request message sent from idle PE to a randomly chosen victim.
struct requestmsg {
  char core[CmiMsgHeaderSizeBytes];
  int from_pe;
};

// ---------------------------------------------------------------------------
// Strategy identification
// ---------------------------------------------------------------------------

void LoadNotifyFn(int /*load*/) {
  // Called by CldTokenHandler after a token is processed locally.
  // Reset the stealing flag so the PE can issue new steal requests once idle
  // again.
  CldIsStealing = false;
}

const char *CldGetStrategy() { return "workstealing"; }

// ---------------------------------------------------------------------------
// Handlers for messages that arrive at this PE carrying work
// ---------------------------------------------------------------------------

// CldHandler: restores the original handler on a message that was wrapped by
// CldSwitchHandler and re-enqueues it in the scheduler priority queue.
static void CldHandler(void *msg) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldRestoreHandler(static_cast<char *>(msg));
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
}

// CldNodeHandler: same as CldHandler but pushes to the node-level queue.
static void CldNodeHandler(void *msg) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldRestoreHandler(static_cast<char *>(msg));
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CmiGetNodeQueue()->push(msg);
}

// CldBalanceHandler: a stolen message has arrived at this PE.
// Put it into the local token queue so it can be processed (or re-stolen).
static void CldBalanceHandler(void *msg) {
  CldRestoreHandler(static_cast<char *>(msg));
  CldPutToken(static_cast<char *>(msg));
}

// ---------------------------------------------------------------------------
// Work-stealing protocol handlers
// ---------------------------------------------------------------------------

// CldAckNoTaskHandler: victim had no stealable tokens; reset the stealing flag
// so this PE will try again on the next idle callback.
static void CldAckNoTaskHandler(void *vmsg) {
  CmiFree(vmsg);
  CldIsStealing = false;
}

// CldAskLoadHandler: runs on the VICTIM when a steal request arrives.
// Extracts one token and forwards it, or sends back an ack-no-task.
static void CldAskLoadHandler(void *vmsg) {
  requestmsg *msg = static_cast<requestmsg *>(vmsg);
  int req_pe = msg->from_pe;
  CmiFree(msg);

  char *stolen = nullptr;
  CldGetToken(&stolen);

  if (stolen != nullptr) {
    int len, queueing, priobits;
    unsigned int *prioptr;
    CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(stolen));
    CldPackFn pfn;
    ifn(stolen, &pfn, &len, &queueing, &priobits, &prioptr);
    // Pack only when crossing a node boundary (intra-node: shared memory).
    if (pfn && CmiNodeOf(req_pe) != CmiMyNode()) {
      pfn(&stolen);
      ifn(stolen, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler(stolen, CldBalanceHandlerIndex);
    CldRelocatedMessages++;
    CmiSyncSendAndFree(req_pe, len, stolen);
  } else {
    // No tokens available; let the requester retry.
    requestmsg *ack =
        static_cast<requestmsg *>(CmiAlloc(sizeof(requestmsg)));
    ack->from_pe = CmiMyPe();
    CmiSetHandler(ack, CldAckNoTaskHandlerIndex);
    CmiSyncSendAndFree(req_pe, sizeof(requestmsg),
                       static_cast<char *>(static_cast<void *>(ack)));
  }
}

// ---------------------------------------------------------------------------
// Idle callback: send a steal request to a random victim
// ---------------------------------------------------------------------------

static void CldStillIdle(void * /*dummy*/) {
  if (CldIsStealing) return;   // already waiting for a response
  if (CmiNumPes() <= 1) return; // nowhere to steal from

  int mype = CmiMyPe();
  int numpes = CmiNumPes();
  int victim;
  do {
    victim = (((CrnRand() + mype) & 0x7FFFFFFF) % numpes);
  } while (victim == mype);

  requestmsg *rmsg =
      static_cast<requestmsg *>(CmiAlloc(sizeof(requestmsg)));
  rmsg->from_pe = mype;
  CmiSetHandler(rmsg, CldAskLoadHandlerIndex);
  CmiSyncSendAndFree(victim, sizeof(requestmsg),
                     static_cast<char *>(static_cast<void *>(rmsg)));
  CldIsStealing = true;
}

// ---------------------------------------------------------------------------
// CldEnqueue / CldNodeEnqueue: main entry points called by Charm++ runtime
// ---------------------------------------------------------------------------

void CldEnqueue(int pe, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;

  if (pe == CLD_ANYWHERE) {
    // Defer placement: put in local token queue so idle PEs can steal.
    ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    // Pack if we may need to send cross-node later; skip for single-node runs.
    if (pfn && CmiNumNodes() > 1) {
      pfn(&msg);
    }
    CmiSetInfo(msg, infofn);
    CldPutToken(static_cast<char *>(msg));
    return;
  }

  ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  if (pe == CmiMyPe() && !CmiImmIsRunning()) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNodeOf(pe) != CmiMyNode()) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler(static_cast<char *>(msg), CpvAccess(CldHandlerIndex));
    CmiSetInfo(msg, infofn);
    if (pe == CLD_BROADCAST)
      CmiSyncBroadcastAndFree(len, msg);
    else if (pe == CLD_BROADCAST_ALL)
      CmiSyncBroadcastAllAndFree(len, msg);
    else
      CmiSyncSendAndFree(pe, len, msg);
  }
}

void CldNodeEnqueue(int node, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;

  if (node == CLD_ANYWHERE) {
    node = CmiMyNode();
  }
  if (node == CmiMyNode() && !CmiImmIsRunning()) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler(static_cast<char *>(msg), CldNodeHandlerIndex);
    CmiSetInfo(msg, infofn);
    if (node == CLD_BROADCAST)
      CmiSyncNodeBroadcastAndFree(len, msg);
    else if (node == CLD_BROADCAST_ALL)
      CmiSyncNodeBroadcastAllAndFree(len, msg);
    else
      CmiSyncNodeSendAndFree(node, len, msg);
  }
}

// ---------------------------------------------------------------------------
// Multi-destination enqueue helpers (unchanged from rand strategy)
// ---------------------------------------------------------------------------

void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler(static_cast<char *>(msg), CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg, infofn);
  CmiSyncMulticastAndFree(grp, len, msg);
}

void CldEnqueueWithinNode(void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldPackFn pfn;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn && !CMI_MSG_NOKEEP(msg)) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler(static_cast<char *>(msg), CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg, infofn);
  CmiWithinNodeBroadcast(len, static_cast<char *>(msg));
}

void CldEnqueueMulti(int npes, const int *pes, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler(static_cast<char *>(msg), CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg, infofn);
  CmiSyncListSendAndFree(npes, pes, len, msg);
}

// ---------------------------------------------------------------------------
// Module initialisation – called once per PE during startup
// ---------------------------------------------------------------------------

void CldModuleInit(char **argv) {
  CpvInitialize(int, CldHandlerIndex);
  CpvAccess(CldHandlerIndex) =
      CmiRegisterHandler(static_cast<CmiHandler>(CldHandler));
  CldNodeHandlerIndex =
      CmiRegisterHandler(static_cast<CmiHandler>(CldNodeHandler));
  CldBalanceHandlerIndex =
      CmiRegisterHandler(static_cast<CmiHandler>(CldBalanceHandler));
  CldAskLoadHandlerIndex =
      CmiRegisterHandler(static_cast<CmiHandler>(CldAskLoadHandler));
  CldAckNoTaskHandlerIndex =
      CmiRegisterHandler(static_cast<CmiHandler>(CldAckNoTaskHandler));

  CldRelocatedMessages = 0;
  CldLoadBalanceMessages = 0;
  CldMessageChunks = 0;

  // Initialise per-PE token queue (registers CldTokenHandler internally).
  CldModuleGeneralInit(argv);

  // Register idle callback for work stealing.
  if (CmiNumPes() > 1) {
    CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
                           static_cast<CcdCondFn>(CldStillIdle), nullptr);
  }
}

void CldCallback() {}
