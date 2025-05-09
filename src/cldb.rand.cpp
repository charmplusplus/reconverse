#include "converse_internal.h"
// #include "queueing.h"
#include "cldb.h"
#include <stdlib.h>

void LoadNotifyFn(int l) {}

const char *CldGetStrategy(void) { return "rand"; }

void CldHandler(char *msg) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldRestoreHandler((char *)msg);
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  // CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
  CmiPushPE(CmiMyPe(), len,
            msg); // use priority queue when we add priority queue
}

void CldNodeHandler(char *msg) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldRestoreHandler(msg);
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  // CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
  CmiGetNodeQueue()->push(msg); // use priority queue when we add priority queue
}

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
  CldSwitchHandler((char *)msg, CldHandlerIndex);
  CmiSetInfo(msg, infofn);

  CmiSyncMulticastAndFree(grp, len, msg);
}

void CldEnqueueWithinNode(void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldPackFn pfn;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);

  // If message is NOKEEP, do not pack it since its pointer is just going to
  // be shared with the other PEs on this node.
  if (pfn && !CMI_MSG_NOKEEP(msg)) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CldHandlerIndex);
  CmiSetInfo(msg, infofn);

  CmiWithinNodeBroadcast(len, (char *)msg);
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
  CldSwitchHandler((char *)msg, CldHandlerIndex);
  CmiSetInfo(msg, infofn);

  CmiSyncListSendAndFree(npes, pes, len, msg);
}

void CldEnqueue(int pe, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  if (pe == CLD_ANYWHERE) {
    pe = (((CrnRand() + CmiMyPe()) & 0x7FFFFFFF) % CmiNumPes());
    /* optimizationfor SMP */
#if CMK_NODE_QUEUE_AVAILABLE
    if (CmiNodeOf(pe) == CmiMyNode()) {
      CldNodeEnqueue(CmiMyNode(), msg, infofn);
      return;
    }
#endif
    if (pe != CmiMyPe())
      CpvAccess(CldRelocatedMessages)++;
  }
  ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  if (pe == CmiMyPe() && !CmiImmIsRunning()) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    /* CsdEnqueueGeneral is not thread or SIGIO safe */
    // CmiPrintf("   myself processor %d ==> %d, length=%d Timer:%f , priori=%d
    // \n", CmiMyPe(), pe, len, CmiWallTimer(), *prioptr);
    CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNodeOf(pe) != CmiMyNode()) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
    CmiSetInfo(msg, infofn);
    if (pe == CLD_BROADCAST) {
      CmiSyncBroadcastAndFree(len, msg);
    } else if (pe == CLD_BROADCAST_ALL) {
      CmiSyncBroadcastAllAndFree(len, msg);
    } else {
      // CmiPrintf("   processor %d ==> %d, length=%d Timer:%f , priori=%d \n",
      // CmiMyPe(), pe, len, CmiWallTimer(), *prioptr);
      CmiSyncSendAndFree(pe, len, msg);
    }
  }
}

void CldNodeEnqueue(int node, void *msg, int infofn) {
  int len, queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  if (node == CLD_ANYWHERE) {
    node = (((CrnRand() + CmiMyNode()) & 0x7FFFFFFF) % CmiNumNodes());
    if (node != CmiMyNode())
      CpvAccess(CldRelocatedMessages)++;
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
    CldSwitchHandler((char *)msg, CpvAccess(CldNodeHandlerIndex));
    CmiSetInfo(msg, infofn);
    if (node == CLD_BROADCAST) {
      CmiSyncNodeBroadcastAndFree(len, msg);
    } else if (node == CLD_BROADCAST_ALL) {
      CmiSyncNodeBroadcastAllAndFree(len, msg);
    } else
      CmiSyncNodeSendAndFree(node, len, msg);
  }
}

void CldModuleInit(char **argv) {
  CpvInitialize(int, CldHandlerIndex);
  CpvAccess(CldHandlerIndex) = CmiRegisterHandler((CmiHandler)CldHandler);
  CpvInitialize(int, CldNodeHandlerIndex);
  CpvAccess(CldNodeHandlerIndex) =
      CmiRegisterHandler((CmiHandler)CldNodeHandler);
  CpvInitialize(int, CldRelocatedMessages);
  CpvInitialize(int, CldLoadBalanceMessages);
  CpvInitialize(int, CldMessageChunks);
  CpvAccess(CldRelocatedMessages) = CpvAccess(CldLoadBalanceMessages) =
      CpvAccess(CldMessageChunks) = 0;
  CldModuleGeneralInit(argv);
}

void CldCallback(void) {}