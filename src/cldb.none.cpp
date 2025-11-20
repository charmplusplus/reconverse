/*
 a cldb which does nothing. Just send the messge locally if the destination
 processor is not specified.
*/

#include "converse_internal.h"
// #include "queueing.h"
#include "cldb.h"
#include <stdlib.h>

void LoadNotifyFn(int l) {}

const char *CldGetStrategy(void) { return "none"; }

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

void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn)
{
  int len, queueing, priobits; unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CldHandlerIndex);
  CmiSetInfo(msg,infofn);

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
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  if (pe == CLD_ANYWHERE) { // should go to new seed balancer
    pe = CmiMyPe();
  }
  if (pe == CmiMyPe() &&
      !CmiImmIsRunning()) { // can probably get rid of CmiImmIsRunning
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    /* CsdEnqueueGeneral is not thread or SIGIO safe */
    // CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
    CmiPushPE(CmiMyPe(), len,
              msg); // use priority queue when we add priority queue
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CldHandlerIndex);
    CmiSetInfo(msg, infofn);
    if (pe == CLD_BROADCAST) {
      CmiSyncBroadcastAndFree(len, msg);
    } else if (pe == CLD_BROADCAST_ALL) {
      CmiSyncBroadcastAllAndFree(len, msg);
    } else
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
    // CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
    CmiGetNodeQueue()->push(
        msg); // use priority queue when we add priority queue
  } else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CldHandlerIndex);
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
  CldHandlerIndex = CmiRegisterHandler((CmiHandler)CldHandler);
  CldRelocatedMessages = 0;
  CldLoadBalanceMessages = 0;
  CldMessageChunks = 0;
  CldModuleGeneralInit(argv);
}

void CldCallback(void) {}