#include "converse.h"

#ifndef _QUIESCENCE_H_
#define _QUIESCENCE_H_

struct ConvQdMsg
{  
  char core[CmiMsgHeaderSizeBytes];
  int phase; /* 0..2*/
  union 
  {
    struct { int64_t created; int64_t processed; } p1;
    struct { char dirty; } p2;
  } u;
};


struct ConvQdState 
{
  int stage; /* 0..2*/
  char cDirty;
  int64_t oProcessed;
  int64_t mCreated, mProcessed;
  int64_t cCreated, cProcessed;
  int nReported;
  int nChildren;
  int parent;
  int *children;
};

typedef struct ConvQdMsg    *CQdMsg;
typedef struct ConvQdState  *CQdState;
typedef CmiHandler CQdCondFn;

void CQdRegisterCallback(CQdCondFn, void *);
void CmiStartQD(CQdCondFn, void *);


/* Declarations for CQdMsg related operations */
int  CQdMsgGetPhase(CQdMsg); 
void CQdMsgSetPhase(CQdMsg, int); 
int64_t CQdMsgGetCreated(CQdMsg);
void CQdMsgSetCreated(CQdMsg, int64_t);
int64_t CQdMsgGetProcessed(CQdMsg);
void CQdMsgSetProcessed(CQdMsg, int64_t);
char CQdMsgGetDirty(CQdMsg); 
void CQdMsgSetDirty(CQdMsg, char); 

/* Declarations for CQdState related operations */
void CQdInit(void);
int64_t CQdGetCreated(CQdState);
void CQdCreate(CQdState, int64_t);
int64_t CQdGetProcessed(CQdState);
void CQdProcess(CQdState, int64_t);
void CQdPropagate(CQdState, CQdMsg); 
int  CQdGetParent(CQdState); 
int64_t CQdGetCCreated(CQdState);
int64_t CQdGetCProcessed(CQdState);
void CQdSubtreeCreate(CQdState, int64_t);
void CQdSubtreeProcess(CQdState, int64_t);
int  CQdGetStage(CQdState); 
void CQdSetStage(CQdState, int); 
void CQdReported(CQdState); 
int  CQdAllReported(CQdState); 
void CQdReset(CQdState); 
void CQdMarkProcessed(CQdState); 
char CQdIsDirty(CQdState); 
void CQdSubtreeSetDirty(CQdState, char); 

CQdState CQdStateCreate(void);
void CQdHandler(CQdMsg);

#endif