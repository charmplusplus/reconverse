#include <unistd.h>
#include "converse.h"

#include "ccs-server.h"
#include "conv-ccs.h"

#if CMK_CCS_AVAILABLE

void CcsHandleRequest(CcsImplHeader *hdr,const char *reqData);

void req_fw_handler(char *msg)
{
  int offset = CmiReservedHeaderSize + sizeof(CcsImplHeader);
  CcsImplHeader *hdr = (CcsImplHeader *)(msg+CmiReservedHeaderSize);

  int destPE = (int)ChMessageInt(hdr->pe);
  //  CmiPrintf("[%d] req_fw_handler got msg for pe %d\n",CmiMyPe(),destPE);
  if (CmiMyPe() == 0 && destPE == -1) {
    /* Broadcast message to all other processors */
    int len=CmiReservedHeaderSize+sizeof(CcsImplHeader)+ChMessageInt(hdr->len);
    CmiSyncBroadcast(len, msg);
  }
  else if (destPE < -1) {
    /* Multicast the message to your children */
    int len=CmiReservedHeaderSize+sizeof(CcsImplHeader)+ChMessageInt(hdr->len)-destPE*sizeof(ChMessageInt_t);
    int index, child, i;
    int *pes = (int*)(msg+CmiReservedHeaderSize+sizeof(CcsImplHeader));
    ChMessageInt_t *pes_nbo = (ChMessageInt_t *)pes;
    offset -= destPE * sizeof(ChMessageInt_t);
    if (ChMessageInt(pes_nbo[0]) == CmiMyPe()) {
      for (index=0; index<-destPE; ++index) pes[index] = ChMessageInt(pes_nbo[index]);
    }
    for (index=0; index<-destPE; ++index) {
      if (pes[index] == CmiMyPe()) break;
    }
    child = (index << 2) + 1;
    for (i=0; i<4; ++i) {
      if (child+i < -destPE) {
        CmiSyncSend(pes[child+i], len, msg);
      }
    }
  }
  
  if(destPE>0 && destPE!=CmiMyPe())
    {
      // The request reached this PE on its way to destPE: forward it.
      int len=CmiReservedHeaderSize+sizeof(CcsImplHeader)+ChMessageInt(hdr->len);
      CmiSyncSend(destPE, len, msg);
    }
  else
    {
      CcsHandleRequest(hdr, msg+offset);
    }
  CmiFree(msg);

}

extern int rep_fw_handler_idx;
/**
 * Decide if the reply is ready to be forwarded to the waiting client,
 * or if combination is required (for broadcast/multicast CCS requests.
 */
int CcsReply(CcsImplHeader *rep,int repLen,const void *repData) {
  int repPE = (int)ChMessageInt(rep->pe);
  if (repPE <= -1) {
    /* Reduce the message to get the final reply */
    CcsHandlerRec *fn;
    int len=CmiReservedHeaderSize+sizeof(CcsImplHeader)+repLen;
    char *msg=(char*)CmiAlloc(len);
    char *r=msg+CmiReservedHeaderSize;
    char *handlerStr;
    rep->len = ChMessageInt_new(repLen);
    *(CcsImplHeader *)r=*rep; r+=sizeof(CcsImplHeader);
    memcpy(r,repData,repLen);
    CmiSetHandler(msg,rep_fw_handler_idx);
    handlerStr=rep->handler;
    fn=(CcsHandlerRec *)CcsGetHandler(handlerStr);
    /* PE 0 checked the handler and its merge function before forwarding
       (CcsImpl_netRequest); a miss here means this PE's handler table differs
       from PE 0's */
    if (fn == NULL || fn->mergeFn == NULL)
      CmiAbort("CCS: broadcast or multicast reply for handler '%s' on PE %d, "
               "which has no such handler or no merge function: register "
               "handlers and merge functions on every PE\n",
               handlerStr, CmiMyPe());
    if (repPE == -1) {
      /* CCS Broadcast */
      CmiReduce(msg, len, fn->mergeFn);
    } else {
      /* CCS Multicast */
      CmiListReduce(-repPE, (int*)(rep+1), msg, len, fn->mergeFn, fn->redID);
    }
  } else {
    CcsImpl_reply(rep, repLen, repData);
  }
  return 0;
}


/**********************************************
  "ccs_getinfo"-- takes no data
    Return the number of parallel nodes, and
      the number of processors per node as an array
      of 4-byte big-endian ints.
*/

void ccs_getinfo(char *msg)
{
  int nNode=CmiNumNodes();
  int len=(1+nNode)*sizeof(ChMessageInt_t);
  ChMessageInt_t *table=(ChMessageInt_t *)malloc(len);
  int n;
  table[0]=ChMessageInt_new(nNode);
  for (n=0;n<nNode;n++)
    table[1+n]=ChMessageInt_new(CmiNodeSize(n));
  CcsSendReply(len,(const char *)table);
  free(table);
  CmiFree(msg);
}

#endif /*CMK_CCS_AVAILABLE*/
