#ifndef CONVERSE_H
#define CONVERSE_H

#include "CpvMacros.h" // for backward compatibility
#include "convcore.h"

typedef void (*CmiStartFn)(int argc, char **argv);
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched = 0, int initret = 0);

static CmiStartFn Cmi_startfn;

// handler tools
typedef void (*CmiHandler)(void *msg);
typedef void (*CmiHandlerEx)(void *msg, void *userPtr);

int CmiRegisterHandler(CmiHandler h);

// state getters
int CmiMyPe();
int CmiMyNode();
int CmiMyNodeSize();
int CmiMyRank();
int CmiNumPes();
int CmiNodeOf(int pe);
int CmiRankOf(int pe);
int CmiStopFlag();

void CmiSetHandler(void *msg, int handlerId);
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

// Cth implementation

#define CQS_QUEUEING_FIFO 2
#define CQS_QUEUEING_LIFO 3
#define CQS_QUEUEING_IFIFO 4
#define CQS_QUEUEING_ILIFO 5
#define CQS_QUEUEING_BFIFO 6
#define CQS_QUEUEING_BLIFO 7
#define CQS_QUEUEING_LFIFO 8
#define CQS_QUEUEING_LLIFO 9


#define OBJ_ID_SZ 4
typedef struct _CmiObjId {
int id[OBJ_ID_SZ];
  /* 
   * **CWL** Note: setting initial values to -1 does not seem to be done for 
   *               LDObjid. Potential consistency problems could arise. This
   *               will probably have to be dealt with later.
   */
#ifdef __cplusplus
  _CmiObjId() { 
    for (int i=0; i<OBJ_ID_SZ; i++) {
      id[i] = -1;
    }
  }
  bool isNull() {
    for (int i=0; i<OBJ_ID_SZ; i++) {
      if (id[i] != -1) return false;
    }
    return true;
  }
  bool operator==(const struct _CmiObjId& objid) const {
    for (int i=0; i<OBJ_ID_SZ; i++) if (id[i] != objid.id[i]) return false;
    return true;
  }
#endif
} CmiObjId;


typedef struct CthThreadStruct *CthThread;
typedef struct {
  /*Start with a message header so threads can be enqueued 
    as messages (e.g., by CthEnqueueNormalThread in convcore.C)
  */
  char cmicore[CmiMsgHeaderSizeBytes];
  CthThread thread;
  int serialNo;
} CthThreadToken;

typedef void        (*CthVoidFn)(void *);
typedef void        (*CthAwkFn)(CthThreadToken *,int,
				int prioBits,unsigned int *prioptr);
typedef CthThread   (*CthThFn)(void);


CthThreadToken *CthGetToken(CthThread);

void CthInit(char **argv);
void CthSchedInit();

CthThread CthSelf(void);

CthThread CthCreate(CthVoidFn fn, void *arg, int size);

static void CthThreadFree(CthThread t);

void CthResume(CthThread t);

void CthSuspend(void);

void CthAwaken(CthThread th);

void CthYield(void);

#endif