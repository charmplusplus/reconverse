#ifndef CONVERSE_H
#define CONVERSE_H

#include "CpvMacros.h" // for backward compatibility

typedef struct Header
{
    int handlerId;
    int messageId;
    int messageSize;
    int destPE;
} CmiMessageHeader;

#define CmiMsgHeaderSizeBytes sizeof(CmiMessageHeader)

typedef void (*CmiStartFn)(int argc, char **argv);
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched = 0, int initret = 0);

static CmiStartFn Cmi_startfn;

// handler tools
typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr);

int CmiRegisterHandler(CmiHandler h);

// message allocation
void *CmiAlloc(int size);
void CmiFree(void *msg);

// state getters
int CmiMyPe();
int CmiMyNode();
int CmiMyNodeSize();
int CmiMyRank();
int CmiNumPes();
int CmiNumNodes();
int CmiNodeOf(int pe);
int CmiRankOf(int pe);
int CmiStopFlag();
#define CmiNodeSize(n) (CmiMyNodeSize())
int CmiNodeFirst(int node);

#define CmiSyncListSend(n,l,s,m)        (CmiSyncListSendFn((n),(l),(s),(char *)(m)))
#define CmiSyncListSendAndFree(n,l,s,m) (CmiFreeListSendFn((n),(l),(s),(char *)(m)))

// handler things
void CmiSetHandler(void *msg, int handlerId);
int CmiGetHandler(void *msg);
CmiHandler CmiGetHandlerFunction(int n);
void CmiHandleMessage(void *msg);

// Barrier functions
void CmiNodeBarrier();
void CmiNodeAllBarrier();

void CsdExitScheduler();

void CmiAbort(const char *format, ...);
void CmiInitCPUTopology(char **argv);
void CmiInitCPUAffinity(char **argv);

double getCurrentTime(void);
double CmiWallTimer(void);

//convconds
#define CcdSCHEDLOOP            0
#define CcdPROCESSOR_BEGIN_BUSY 1
#define CcdPROCESSOR_END_IDLE   1 /*Synonym*/
#define CcdPROCESSOR_BEGIN_IDLE 2
#define CcdPROCESSOR_END_BUSY   2 /*Synonym*/
#define CcdPROCESSOR_STILL_IDLE 3
#define CcdPROCESSOR_LONG_IDLE  4

/*Periodic calls*/
#define CcdPERIODIC_FIRST     5 /*first periodic value*/
#define CcdPERIODIC           5 /*every few ms*/
#define CcdPERIODIC_10ms      6 /*every 10ms (100Hz)*/
#define CcdPERIODIC_100ms     7 /*every 100ms (10Hz)*/
#define CcdPERIODIC_1second   8 /*every second*/
#define CcdPERIODIC_1s        8 /*every second*/
#define CcdPERIODIC_5s        9 /*every second*/
#define CcdPERIODIC_5seconds  9 /*every second*/
#define CcdPERIODIC_10second  10 /*every 10 seconds*/
#define CcdPERIODIC_10seconds 10 /*every 10 seconds*/
#define CcdPERIODIC_10s       10 /*every 10 seconds*/
#define CcdPERIODIC_1minute   11 /*every minute*/
#define CcdPERIODIC_2minute   12 /*every 2 minute*/
#define CcdPERIODIC_5minute   13 /*every 5 minute*/
#define CcdPERIODIC_10minute  14 /*every 10 minutes*/
#define CcdPERIODIC_1hour     15 /*every hour*/
#define CcdPERIODIC_12hour    16 /*every 12 hours*/
#define CcdPERIODIC_1day      17 /*every day*/
#define CcdPERIODIC_LAST      18 /*last periodic value +1*/

/*Other conditions*/
#define CcdQUIESCENCE        18
#define CcdTOPOLOGY_AVAIL    19
#define CcdSIGUSR1           20
#define CcdSIGUSR2           21

/*User-defined conditions start here*/
#define CcdUSER              22
#define CcdUSERMAX          127

//convcond functions
void CcdModuleInit();
void CcdCallFnAfter(CmiHandler fnp, void *arg, double msecs);
int CcdCallOnCondition(int condnum, CmiHandler fnp, void *arg);
int CcdCallOnConditionKeep(int condnum, CmiHandler fnp, void *arg);
void CcdCancelCallOnCondition(int condnum, int idx);
void CcdCancelCallOnConditionKeep(int condnum, int idx);
void CcdRaiseCondition(int condnum);
void CcdCallBacks(void);

//error checking
#define CmiAssert(expr) ((void)0)
#define CmiAssertMsg(expr, ...) ((void)0)
#define _MEMCHECK(p) do{}while(0)

//spantree
//later: fix the naming of these macros to be clearer
#define CMK_SPANTREE_MAXSPAN 4
#define CST_W  (CMK_SPANTREE_MAXSPAN)
#define CST_NN (CmiNumNodes())
#define CmiNodeSpanTreeParent(n) ((n)?(((n)-1)/CST_W):(-1))
#define CmiNodeSpanTreeChildren(n,c) do {\
          int _i; \
          for(_i=0; _i<CST_W; _i++) { \
            int _x = (n)*CST_W+_i+1; \
            if(_x<CST_NN) (c)[_i]=_x; \
          }\
        } while(0)
#define CmiNumNodeSpanTreeChildren(n) ((((n)+1)*CST_W<CST_NN)? CST_W : \
          ((((n)*CST_W+1)>=CST_NN)?0:((CST_NN-1)-(n)*CST_W)))
#define CST_R(p) (CmiRankOf(p))
#define CST_NF(n) (CmiNodeFirst(n))
#define CST_SP(n) (CmiNodeSpanTreeParent(n))
#define CST_ND(p) (CmiNodeOf(p))
#define CST_NS(p) (CmiNodeSize(CST_ND(p)))
#define CmiSpanTreeParent(p) ((p)?(CST_R(p)?(CST_NF(CST_ND(p))+(CST_R(p)-1)/CST_W):CST_NF(CST_SP(CST_ND(p)))):(-1))
#define CST_C(p) (((CST_R(p)+1)*CST_W<CST_NS(p))?CST_W:(((CST_R(p)*CST_W+1)>=CST_NS(p))?0:((CST_NS(p)-1)-CST_R(p)*CST_W)))
#define CST_SC(p) (CmiNumNodeSpanTreeChildren(CST_ND(p)))
#define CmiNumSpanTreeChildren(p) (CST_R(p)?CST_C(p):(CST_SC(p)+CST_C(p)))
#define CmiSpanTreeChildren(p,c) do {\
          int _i,_c=0; \
          if(CST_R(p)==0) { \
            for(_i=0;_i<CST_W;_i++) { \
              int _x = CST_ND(p)*CST_W+_i+1; \
              if(_x<CST_NN) (c)[_c++]=CST_NF(_x); \
            }\
          } \
          for(_i=0;_i<CST_W;_i++) { \
            int _x = CST_R(p)*CST_W+_i+1; \
            if(_x<CST_NS(p)) (c)[_c++]=CST_NF(CST_ND(p))+_x; \
          }\
        } while(0)

#endif