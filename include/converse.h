//function declarations that a user program can call 

#ifndef CONVERSE_H
#define CONVERSE_H

#include <cinttypes>
#include <cstdlib>
#include <cstdio>

using CmiInt1 = std::int8_t;
using CmiInt2 = std::int16_t;
using CmiInt4 = std::int32_t;
using CmiInt8 = std::int64_t;
using CmiUint1 = std::uint8_t;
using CmiUint2 = std::uint16_t;
using CmiUint4 = std::uint32_t;
using CmiUint8 = std::uint64_t;

// NOTE: these are solely for backwards compatibility
// Do not use in reconverse impl

#define CMK_TAG(x, y) x##y##_

#define CpvDeclare(t, v) t *CMK_TAG(Cpv_, v)
#define CpvStaticDeclare(t, v) static t *CMK_TAG(Cpv_, v)

#define CpvInitialize(t, v)                            \
    do                                                 \
    {                                                  \
        if (false /* I don't understand */)            \
        {                                              \
            CmiNodeBarrier();                          \
        }                                              \
        else                                           \
        {                                              \
            CMK_TAG(Cpv_, v) = new t[CmiMyNodeSize()]; \
            CmiNodeBarrier();                          \
        }                                              \
    } while (0)
;

#define CpvAccess(v) CMK_TAG(Cpv_, v)[CmiMyRank()]

// End of NOTE

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

// handler things
void CmiSetHandler(void *msg, int handlerId);
int CmiGetHandler(void *msg);
CmiHandler CmiGetHandlerFunction(int n);
void CmiHandleMessage(void *msg);

// message sending
void CmiSyncSend(int destPE, int messageSize, void *msg);
void CmiSyncSendAndFree(int destPE, int messageSize, void *msg);

// broadcasts
void CmiSyncBroadcast(int size, void *msg);
void CmiSyncBroadcastAndFree(int size, void *msg);
void CmiSyncBroadcastAll(int size, void *msg);
void CmiSyncBroadcastAllAndFree(int size, void *msg);
void CmiSyncNodeSendAndFree(unsigned int destNode, unsigned int size, void *msg);

// Barrier functions
void CmiNodeBarrier();
void CmiNodeAllBarrier();

void CsdExitScheduler();

// Exit functions 
void CmiExit(int status);
void CmiAbort(const char *format, ...);

// Utility functions
int CmiPrintf(const char *format, ...);
int CmiGetArgc(char **argv);
void CmiAbort(const char *format, ...);

void CmiInitCPUTopology(char **argv);
void CmiInitCPUAffinity(char **argv);

// Timer/time related functions
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

/* Command-Line-Argument handling */
void CmiArgGroup(const char *parentName,const char *groupName);
int CmiGetArgInt(char **argv,const char *arg,int *optDest);
int CmiGetArgIntDesc(char **argv,const char *arg,int *optDest,const char *desc);
int CmiGetArgLong(char **argv,const char *arg,CmiInt8 *optDest);
int CmiGetArgLongDesc(char **argv,const char *arg,CmiInt8 *optDest,const char *desc);
int CmiGetArgDouble(char **argv,const char *arg,double *optDest);
int CmiGetArgDoubleDesc(char **argv,const char *arg,double *optDest,const char *desc);
int CmiGetArgString(char **argv,const char *arg,char **optDest);
int CmiGetArgStringDesc(char **argv,const char *arg,char **optDest,const char *desc);
int CmiGetArgFlag(char **argv,const char *arg);
int CmiGetArgFlagDesc(char **argv,const char *arg,const char *desc);
void CmiDeleteArgs(char **argv,int k);
int CmiGetArgc(char **argv);
char **CmiCopyArgs(char **argv);
int CmiArgGivingUsage(void);
void CmiDeprecateArgInt(char **argv,const char *arg,const char *desc,const char *warning);

//error checking

//do we want asserts to be defaulted to be on or off(right now it is on)
#ifndef CMK_OPTIMIZE
  #define CMK_OPTIMIZE 0 
#endif

#if CMK_OPTIMIZE 
  #define CmiAssert(expr) ((void)0)
  #define CmiAssertMsg(expr, ...) ((void)0)
#else 
  #define CmiAssert(expr) do {                                                                 \
      if (!(expr)) {                                                                           \
        fprintf(stderr, "Assertion %s failed: file %s, line %d\n", #expr, __FILE__, __LINE__); \
        CmiExit(0);                                                                            \
      }                                                                                        \
  } while (0)

  #define CmiAssertMsg(expr, ...) do {    \
    if (!(expr)) {                        \
      fprintf(stderr, __VA_ARGS__);       \
      fprintf(stderr, "\n");              \
      CmiExit(0);                         \
    }                                     \
  } while (0)
#endif 

#define _MEMCHECK(p) do{ \
  if (!p) { \
    fprintf(stderr, "Memory allocation check failed: %s:%d\n", __FILE__, __LINE__); \
    abort(); \
  } \
} while(0)

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