#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "converse.h"
#include "conv-ccs.h"
#include "ccs-server.h"
#include "sockRoutines.h"

#if CMK_CCS_AVAILABLE

int CcsReply(CcsImplHeader *rep,int repLen,const void *repData);

/*****************************************************************************
 *
 * Converse Client-Server Functions
 *
 *****************************************************************************/
 
static void initHandlerRec(CcsHandlerRec *c,const char *name) {
  if (strlen(name)>=CCS_MAXHANDLER) 
  	CmiAbort("CCS handler names cannot exceed 32 characters");
  c->name=strdup(name);
  c->fn=NULL;
  c->fnOld=NULL;
  c->userPtr=NULL;
  c->mergeFn=NULL;
  c->nCalls=0;
}

#if CMK_CHARM4PY
static void callHandlerRec(CcsHandlerRec *c, int reqLen, const void *reqData)
{
  c->nCalls++;
  //structure of message: reserved header + handler name length (up to 32) + data length (up to 1024)
  //+handler name + data
  //get handler name and length (up to 32)
  const char *handlerStr = c->name;
  const int handlerStrLen = strlen(handlerStr);
  //reserved header name and data
  char *cmsg = (char *) CmiAlloc(CmiReservedHeaderSize+2*sizeof(int)+CCS_MAXHANDLER+reqLen);
  //handler length (up to 32)
  memcpy(cmsg+CmiReservedHeaderSize, &handlerStrLen, sizeof(int));
  //data length (up to 1024)
  memcpy(cmsg+CmiReservedHeaderSize+sizeof(int), &reqLen, sizeof(int));
  //handler name (up to 32)
  memcpy(cmsg+CmiReservedHeaderSize+2*sizeof(int), handlerStr, handlerStrLen);
  //data
  memcpy(cmsg+CmiReservedHeaderSize+2*sizeof(int)+CCS_MAXHANDLER, reqData, reqLen);
  (c->fnOld)(cmsg);
}

#else
static void callHandlerRec(CcsHandlerRec *c,int reqLen,const void *reqData) {
	c->nCalls++;
	if (c->fnOld) 
	{ /* Backward compatability version:
	    Pack user data into a converse message (cripes! why bother?);
	    user will delete the message. 
	  */
		char *cmsg = (char *) CmiAlloc(CmiReservedHeaderSize+reqLen);
		memcpy(cmsg+CmiReservedHeaderSize, reqData, reqLen);
		(c->fnOld)(cmsg);
	}
	else { /* Pass read-only copy of data straight to user */
		(c->fn)(c->userPtr, reqLen, reqData);
	}
}

#endif

/*Table maps handler name to CcsHandler object. Pointers into an
  unordered_map stay valid across later insertions, so CcsGetHandler can
  hand them out.*/
typedef std::unordered_map<std::string, CcsHandlerRec> CcsHandlerTable;
CpvStaticDeclare(CcsHandlerTable *, ccsTab);

static void putHandlerRec(const CcsHandlerRec &cp) {
  (*CpvAccess(ccsTab))[cp.name] = cp;
}
static CcsHandlerRec *getHandlerRec(const char *name) {
  CcsHandlerTable &tab = *CpvAccess(ccsTab);
  auto it = tab.find(name);
  return it == tab.end() ? NULL : &it->second;
}

CpvStaticDeclare(CcsImplHeader*,ccsReq);/*Identifies CCS requestor (client)*/

void CcsRegisterHandler(const char *name, CmiHandler fn) {
  CcsHandlerRec cp;
  initHandlerRec(&cp,name);
  cp.fnOld=fn;
  putHandlerRec(cp);
}

#ifdef CMK_CHARM4PY
void CcsRegisterHandlerExt(const char *ccs_handlername, void *fn)
{
  CcsRegisterHandler(ccs_handlername, (CmiHandler)fn);
}
#endif

void CcsRegisterHandlerFn(const char *name, CcsHandlerFn fn, void *ptr) {
  CcsHandlerRec cp;
  initHandlerRec(&cp,name);
  cp.fn=fn;
  cp.userPtr=ptr;
  putHandlerRec(cp);
}
CcsHandlerRec *CcsGetHandler(const char *name) {
  return getHandlerRec(name);
}
void CcsSetMergeFn(const char *name, CmiReduceMergeFn newMerge) {
  CcsHandlerRec *rec=getHandlerRec(name);
  if (rec==NULL) {
    CmiAbort("[%d] CCS: Unknown CCS handler name %s.\n",CmiMyPe(),name);
  }
  rec->mergeFn=newMerge;
  /* Every PE calls CcsSetMergeFn for the same handlers in the same order, so
     this id is the same on all of them: a multicast request's replies reduce
     under it (CmiListReduce in CcsReply). */
  rec->redID=CmiGetGlobalReduction();
}

void * CcsMerge_concat(int *size,void *local,void **remote,int n) {
  CcsImplHeader *hdr;
  int total = *size;
  void *reply;
  char *ptr;
  int i;
  for (i=0; i<n; ++i) {
    hdr = (CcsImplHeader*)(((char*)remote[i])+CmiReservedHeaderSize);
    total += ChMessageInt(hdr->len);
  }
  reply = CmiAlloc(total);
  memcpy(reply, local, *size);
  ((CcsImplHeader*)(((char*)reply)+CmiReservedHeaderSize))->len = ChMessageInt_new(total-CmiReservedHeaderSize-sizeof(CcsImplHeader));
  CmiFree(local);
  ptr = ((char*)reply)+*size;
  for (i=0; i<n; ++i) {
    int len = ChMessageInt(((CcsImplHeader*)(((char*)remote[i])+CmiReservedHeaderSize))->len);
    memcpy(ptr, ((char*)remote[i])+CmiReservedHeaderSize+sizeof(CcsImplHeader), len);
    ptr += len;
  }
  *size = total;
  return reply;
}

#define SIMPLE_REDUCTION(name, dataType, loop) \
void * CcsMerge_##name(int *size,void *local,void **remote,int n) { \
  int i, m; \
  CcsImplHeader *hdrLocal = (CcsImplHeader*)(((char*)local)+CmiReservedHeaderSize); \
  int lenLocal = ChMessageInt(hdrLocal->len); \
  int nElem = lenLocal / sizeof(dataType); \
  dataType *ret = (dataType *) (hdrLocal+1); \
  CcsImplHeader *hdr; \
  for (m=0; m<n; ++m) { \
    int len; \
    dataType *value; \
    hdr = (CcsImplHeader*)(((char*)remote[m])+CmiReservedHeaderSize); \
    len = ChMessageInt(hdr->len); \
    value = (dataType *)(hdr+1); \
    CmiAssert(lenLocal == len); \
    for (i=0; i<nElem; ++i) loop; \
  } \
  return local; \
}

SIMPLE_REDUCTION(logical_and, int, ret[i]=(ret[i]&&value[i])?1:0)
SIMPLE_REDUCTION(logical_or, int, ret[i]=(ret[i]||value[i])?1:0)
SIMPLE_REDUCTION(bitvec_and, int, ret[i]&=value[i])
SIMPLE_REDUCTION(bitvec_or, int, ret[i]|=value[i])

/*Use this macro for reductions that have the same type for all inputs */
#define SIMPLE_POLYMORPH_REDUCTION(nameBase,loop) \
  SIMPLE_REDUCTION(nameBase##_int, int, loop) \
  SIMPLE_REDUCTION(nameBase##_float, float, loop) \
  SIMPLE_REDUCTION(nameBase##_double, double, loop)

SIMPLE_POLYMORPH_REDUCTION(sum, ret[i]+=value[i])
SIMPLE_POLYMORPH_REDUCTION(product, ret[i]*=value[i])
SIMPLE_POLYMORPH_REDUCTION(max, if (ret[i]<value[i]) ret[i]=value[i])
SIMPLE_POLYMORPH_REDUCTION(min, if (ret[i]>value[i]) ret[i]=value[i])

#undef SIMPLE_REDUCTION
#undef SIMPLE_POLYMORPH_REDUCTION

int CcsEnabled(void)
{
  return 1;
}

int CcsIsRemoteRequest(void)
{
  return CpvAccess(ccsReq)!=NULL;
}

void CcsCallerId(skt_ip_t *pip, unsigned int *pport)
{
  *pip = CpvAccess(ccsReq)->attr.ip;
  *pport = ChMessageInt(CpvAccess(ccsReq)->attr.port);
}

extern int rep_fw_handler_idx;
int rep_fw_handler_idx;

CcsDelayedReply CcsDelayReply(void)
{
  CcsDelayedReply ret;
  int len = sizeof(CcsImplHeader);
  /* ChMessageInt is unsigned: compare the PE as an int. A multicast header
     (pe < -1) is followed by its list of -pe PEs, which CcsReply reads. */
  int pe = (int)ChMessageInt(CpvAccess(ccsReq)->pe);
  if (pe < -1)
    len += -pe * (int)sizeof(int);
  ret.hdr = (CcsImplHeader*)malloc(len);
  memcpy(ret.hdr, CpvAccess(ccsReq), len);
  CpvAccess(ccsReq)=NULL;
  return ret;
}

void CcsSendReply(int replyLen, const void *replyData)
{
  if (CpvAccess(ccsReq)==NULL)
    CmiAbort("CcsSendReply: reply already sent!\n");
  CpvAccess(ccsReq)->len = ChMessageInt_new(1);
  CcsReply(CpvAccess(ccsReq),replyLen,replyData);
  CpvAccess(ccsReq) = NULL;
}

void CcsSendReplyNoError(int replyLen, const void *replyData) {
  if (CpvAccess(ccsReq)==NULL) return;
  CcsSendReply(replyLen, replyData);
}

void CcsSendDelayedReply(CcsDelayedReply d,int replyLen, const void *replyData)
{
  CcsImplHeader *h = d.hdr;
  h->len=ChMessageInt_new(1);
  CcsReply(h,replyLen,replyData);
  free(h);
}

void CcsNoReply(void)
{
  if (CpvAccess(ccsReq)==NULL) return;
  CpvAccess(ccsReq)->len = ChMessageInt_new(0);
  CcsReply(CpvAccess(ccsReq),0,NULL);
  CpvAccess(ccsReq) = NULL;
}

void CcsNoDelayedReply(CcsDelayedReply d)
{
  CcsImplHeader *h = d.hdr;
  h->len = ChMessageInt_new(0);
  CcsReply(h,0,NULL);
  free(h);
}


/**********************************
_CCS Implementation Routines:
  These do the request forwarding and
delivery.
***********************************/

/*CCS Bottleneck:
  Deliver the given message data to the given
CCS handler.
*/
void CcsHandleRequest(CcsImplHeader *hdr,const char *reqData)
{
  int reqLen=ChMessageInt(hdr->len);
/*Look up handler's converse ID*/
  CcsHandlerRec *fn=getHandlerRec(hdr->handler);
  //  CmiPrintf("[%d] CCS: CCS handler name '%s' requested.\n",CmiMyPe(), hdr->handler);
  if (fn==NULL) {
    CmiPrintf("[%d] CCS: Unknown CCS handler name '%s' requested. Ignoring...\n"
	      ,CmiMyPe(), hdr->handler);
    CpvAccess(ccsReq)=hdr;
    CcsSendReply(0,NULL); /*Send an empty reply to the possibly waiting client*/
    return;
 /*   CmiAbort("CCS: Unknown CCS handler name.\n");*/
  }

/* Call the handler */
  CpvAccess(ccsReq)=hdr;
  callHandlerRec(fn,reqLen,reqData);

/*Check if a reply was sent*/
  if (CpvAccess(ccsReq)!=NULL)
    CcsSendReply(0,NULL);/*Send an empty reply if not*/
}

/*Unpacks request message to call above routine*/
int _ccsHandlerIdx = 0;/*Converse handler index of routine req_fw_handler*/

void req_fw_handler(char *msg);
void ccs_getinfo(char *msg); /* middle-ccs.cpp */

/*Convert CCS header & message data into a converse message 
 addressed to handler*/
char *CcsImpl_ccs2converse(const CcsImplHeader *hdr,const void *data,int *ret_len)
{
  int reqLen=ChMessageInt(hdr->len);
  int destPE = ChMessageInt(hdr->pe);
  int len;
  char *msg;
  if (destPE < -1) reqLen -= destPE*sizeof(int);
  len=CmiReservedHeaderSize+sizeof(CcsImplHeader)+reqLen;
  msg=(char *)CmiAlloc(len);
  memcpy(msg+CmiReservedHeaderSize,hdr,sizeof(CcsImplHeader));
  memcpy(msg+CmiReservedHeaderSize+sizeof(CcsImplHeader),data,reqLen);
  if (ret_len!=NULL) *ret_len=len;
  CmiSetHandler(msg, _ccsHandlerIdx);
  return msg;
}

/*Receives reply messages passed up from
converse to node 0.*/
static void rep_fw_handler(char *msg)
{
  int len;
  char *r=msg+CmiReservedHeaderSize;
  CcsImplHeader *hdr=(CcsImplHeader *)r; 
  r+=sizeof(CcsImplHeader);
  len=ChMessageInt(hdr->len);
  CcsImpl_reply(hdr,len,r);
  CmiFree(msg);
}

/************** CCS server on PE 0 ***********
There is no conv-host (charmrun) program, so PE 0
carries out the CCS forwarding that conv-host does
on classic netlrts builds.

CCS works by listening to a TCP connection on a 
port-- the Ccs server socket.  A typical communcation
pattern is:

1.) Random program (CCS client) from the net
connects to the CCS server socket and sends
a CCS request.

2.) Node 0 forwards the request to the proper
PE as a regular converse message (built in CcsImpl_netReq)
for CcsHandleRequest.

3.) CcsHandleRequest looks up the user's pre-registered
CCS handler, and passes the user's handler the request data.

4.) The user's handler calls CcsSendReply with some
reply data; OR finishes without calling CcsSendReply,
in which case CcsHandleRequest does it.

5.) CcsSendReply forwards the reply back to node 0,
which sends the reply back to the original requestor,
on the (still-open) request socket.
 */

/**
Send a Ccs reply back to the requestor, down the given socket.
Since there is no conv-host, node 0 does all the CCS 
communication-- this means all requests come to node 0
and are forwarded out; all replies are forwarded back to node 0.

*/
void CcsImpl_reply(CcsImplHeader *rep,int repLen,const void *repData)
{
  const int repPE=0;
  rep->len=ChMessageInt_new(repLen);
  if (CmiMyPe()==repPE) {
    /*Actually deliver reply data*/
    CcsServer_sendReply(rep,repLen,repData);
  } else {
    /*Forward data & socket # to the replyPE*/
    int len=CmiReservedHeaderSize+
           sizeof(CcsImplHeader)+repLen;
    char *msg = (char *)CmiAlloc(len);
    char *r=msg+CmiReservedHeaderSize;
    *(CcsImplHeader *)r=*rep; r+=sizeof(CcsImplHeader);
    memcpy(r,repData,repLen);
    CmiSetHandler(msg,rep_fw_handler_idx);
    CmiSyncSendAndFree(repPE,len,msg);
  }
}

/*No request will be sent through this socket.
Closes it.
*/
/*void CcsImpl_noReply(CcsImplHeader *hdr)
{
  int fd=ChMessageInt(hdr->replyFd);
  skt_close(fd);
}*/

/**
 * This is the entrance point of a CCS request into the server.
 * It is executed only on proc 0, and it forwards the request to the appropriate PE.
 */
/* Turn a request down on PE 0 before it is forwarded: its socket is closed
   without a reply, as for any empty reply. This goes to CcsImpl_reply
   directly, not through CcsSendReply, because CcsReply would start the
   reply reduction of a broadcast or multicast request, which a request
   that never reaches the other PEs cannot finish. */
static void CcsRejectRequest(CcsImplHeader *hdr,const char *why)
{
  CmiPrintf("CCS: request for handler '%.*s' to PE %d rejected: %s\n",
            CCS_MAXHANDLER, hdr->handler, (int)ChMessageInt(hdr->pe), why);
  CcsImpl_reply(hdr,0,NULL);
}

void CcsImpl_netRequest(CcsImplHeader *hdr,const void *reqData)
{
  char *msg;
  int len,repPE=ChMessageInt(hdr->pe);
  /* The handler name comes off the wire; the lookups below need it to end
     within the field. */
  if (memchr(hdr->handler, 0, CCS_MAXHANDLER) == NULL) {
    CcsRejectRequest(hdr, "handler name too long");
    return;
  }
  if (repPE<=-CmiNumPes() || repPE>=CmiNumPes()) {
    /*Treat out of bound values as errors. Helps detecting bugs*/
    CcsRejectRequest(hdr, repPE==-CmiNumPes()
        ? "invalid processor index (a multicast to every PE is a broadcast, pe -1)"
        : "invalid processor index");
    return;
  }
  if (repPE <= -1) {
    /* Every PE's reply is merged with the handler's merge function. The
       handler table is the same on every PE, so PE 0 can check it here. */
    CcsHandlerRec *rec = getHandlerRec(hdr->handler);
    if (rec == NULL) {
      CcsRejectRequest(hdr, "unknown handler");
      return;
    }
    if (rec->mergeFn == NULL) {
      CcsRejectRequest(hdr, "broadcast or multicast to a handler without a merge function (CcsSetMergeFn)");
      return;
    }
  }
  if (repPE < -1) {
    /* The multicast's PE list, at the start of reqData in network byte
       order: each PE must exist and appear once, or the requests and the
       reply reduction would go astray. */
    const ChMessageInt_t *list = (const ChMessageInt_t *)reqData;
    std::vector<char> listed(CmiNumPes(), 0);
    for (int i = 0; i < -repPE; ++i) {
      int pe = (int)ChMessageInt(list[i]);
      if (pe < 0 || pe >= CmiNumPes()) {
        CcsRejectRequest(hdr, "multicast lists an invalid processor index");
        return;
      }
      if (listed[pe]) {
        CcsRejectRequest(hdr, "multicast lists a processor twice");
        return;
      }
      listed[pe] = 1;
    }
  }

  msg=CcsImpl_ccs2converse(hdr,reqData,&len);
  if (repPE >= 0) {
    CmiSyncSendAndFree(repPE,len,msg);
  } else if (repPE == -1) {
    /* Broadcast to all processors */
    CmiSyncSendAndFree(0,len,msg);
  } else {
    /* Multicast to -repPE processors, specified right at the beginning of reqData (as a list of pes) */
    int firstPE = ChMessageInt(*(ChMessageInt_t*)reqData);
    CmiSyncSendAndFree(firstPE,len,msg);
  }
}

/*
PE 0 runs the CCS server socket. To keep the speed impact minimal,
it only probes for new connections (with CcsServerCheck) from the
CcdPERIODIC condition.
 */
static void CcsServerCheck(void *)
{
  while (1==skt_select1(CcsServer_fd(),0)) {
    CcsImplHeader hdr;
    void *data;
    if (CcsServer_recvRequest(&hdr,&data))
    {/*We got a network request*/
      CcsImpl_netRequest(&hdr,data);
      free(data);
    }
  }
}

/* Runs on every PE from ConverseInit, before the start function. The handler
   registrations happen in the same order on every PE, so the indices agree
   across PEs; the server socket is opened on PE 0 only, and only when the
   program was started with ++server, ++server-port or ++server-auth. */
void CcsInit(char **argv)
{
  CpvInitialize(CcsHandlerTable *, ccsTab);
  CpvAccess(ccsTab) = new CcsHandlerTable();
  CpvInitialize(CcsImplHeader *, ccsReq);
  CpvAccess(ccsReq) = NULL;
  CmiAssignOnce(&_ccsHandlerIdx,CmiRegisterHandler((CmiHandler)req_fw_handler));
  CmiAssignOnce(&rep_fw_handler_idx, CmiRegisterHandler((CmiHandler)rep_fw_handler));
  /* The one builtin request the runtime answers itself: node and PE counts.
     Every client sends it first (CcsConnect). */
  CcsRegisterHandler("ccs_getinfo",(CmiHandler)ccs_getinfo);

  int ccs_serverPort=0;
  char *ccs_serverAuth=NULL;
  if (CmiGetArgFlagDesc(argv,"++server", "Create a CCS server port") |
      CmiGetArgIntDesc(argv,"++server-port",&ccs_serverPort, "Listen on this TCP/IP port number") |
      CmiGetArgStringDesc(argv,"++server-auth",&ccs_serverAuth, "Use this CCS authentication file"))
    if (CmiMyPe()==0)
    {/*Create and occasionally poll on a CCS server port*/
      CcsServer_new(NULL,&ccs_serverPort,ccs_serverAuth);
      CcdCallOnConditionKeep(CcdPERIODIC,CcsServerCheck,NULL);
    }
}

#endif /*CMK_CCS_AVAILABLE*/

