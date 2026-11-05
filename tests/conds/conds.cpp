#include "converse.h"
#include <stdio.h>

CpvDeclare(int, test);
CpvDeclare(int, exitHandlerId);
CpvDeclare(int, cancelledIdx);
CpvDeclare(int, cancelledKeepIdx);
CpvDeclare(int, survivorFired);
CpvDeclare(int, survivorKeepFired);
CpvDeclare(int, remoteFired);
CpvDeclare(int, nodeHandlerId);
CpvDeclare(int, 1sHandlerId);
CpvDeclare(int, 5sHandlerId);
CpvDeclare(int, 10sHandlerId);

struct Message {
  CmiMessageHeader header;
};

void stop_handler(void *vmsg) { CsdExitScheduler(); }

void shortHandler(void *vmsg) {
  printf("1s HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(),
         CmiMyRank());
}

void mediumHandler(void *vmsg) {
  printf("5s HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(),
         CmiMyRank());
}

void callAfter7s(void *vmsg, double) {
  printf("7s (ccd call after) HANDLER CALLED at time %lf on PE %d\n",
         CmiWallTimer(), CmiMyRank());
}

void longHandler(void *vmsg) {
  printf("10s HANDLER CALLED at time %lf on PE %d\n", CmiWallTimer(),
         CmiMyRank());
  if (!CpvAccess(remoteFired))
    CmiAbort("conds: no CcdCallFnAfterOnPE callback reached PE %d in 10 s",
             CmiMyPe());
  if (CpvAccess(survivorFired) != 1)
    CmiAbort("conds: the callback registered between two cancelled ones fired "
             "%d times on PE %d, expected once", CpvAccess(survivorFired),
             CmiMyPe());
  if (CpvAccess(survivorKeepFired) < 1)
    CmiAbort("conds: the keep callback registered between two cancelled ones "
             "never fired on PE %d", CmiMyPe());
  Message *msg = (Message *)CmiAlloc(sizeof(Message));
  msg->header.handlerId = CpvAccess(exitHandlerId);
  msg->header.messageSize = sizeof(Message);
  msg->header.destPE = CmiMyRank();
  CmiSyncSendAndFree(CmiMyRank(), msg->header.messageSize, msg);
}

// Cancelled callbacks must never fire. CcdCallFnAfterOnPE runs its callback
// on the registering PE after the delay, as classic Converse does on SMP
// builds (the pe argument only set the apparent PE for the old
// single-thread emulation); the test pins that behaviour.
void cancelledHandler(void *) {
  CmiAbort("conds: a cancelled CcdCallOnCondition callback fired on PE %d",
           CmiMyPe());
}
void cancelledKeepHandler(void *) {
  CmiAbort("conds: a cancelled CcdCallOnConditionKeep callback fired on PE %d",
           CmiMyPe());
}
// Registered between two callbacks that are then cancelled in registration
// order: a handle must still name its own callback after an earlier one is
// removed (CkArray cancels its spring-cleaning callback in its destructor, and
// arrays are destroyed in any order).
void survivorHandler(void *) { CpvAccess(survivorFired)++; }
void survivorKeepHandler(void *) { CpvAccess(survivorKeepFired)++; }
void remoteAfter(void *arg, double) {
  int registrant = (int)(size_t)arg;
  if (CmiMyPe() != registrant)
    CmiAbort("conds: CcdCallFnAfterOnPE registered on PE %d ran on PE %d",
             registrant, CmiMyPe());
  CpvAccess(remoteFired) = 1;
  printf("CcdCallFnAfterOnPE callback ran on PE %d at %lf\n", CmiMyPe(),
         CmiWallTimer());
}

CmiStartFn mymain(int argc, char **argv) {
  CpvInitialize(int, cancelledIdx);
  CpvInitialize(int, cancelledKeepIdx);
  CpvInitialize(int, remoteFired);
  CpvAccess(remoteFired) = 0;
  CpvAccess(cancelledIdx) =
      CcdCallOnCondition(CcdPERIODIC_1s, cancelledHandler, 0);
  CcdCancelCallOnCondition(CcdPERIODIC_1s, CpvAccess(cancelledIdx));
  CpvAccess(cancelledKeepIdx) =
      CcdCallOnConditionKeep(CcdPERIODIC_1s, cancelledKeepHandler, 0);
  CcdCancelCallOnConditionKeep(CcdPERIODIC_1s, CpvAccess(cancelledKeepIdx));
  // Three registrations; the first and the third are cancelled, in that order.
  CpvInitialize(int, survivorFired);
  CpvInitialize(int, survivorKeepFired);
  CpvAccess(survivorFired) = 0;
  CpvAccess(survivorKeepFired) = 0;
  int first = CcdCallOnCondition(CcdPERIODIC_1s, cancelledHandler, 0);
  CcdCallOnCondition(CcdPERIODIC_1s, survivorHandler, 0);
  int third = CcdCallOnCondition(CcdPERIODIC_1s, cancelledHandler, 0);
  CcdCancelCallOnCondition(CcdPERIODIC_1s, first);
  CcdCancelCallOnCondition(CcdPERIODIC_1s, third);
  int firstKeep = CcdCallOnConditionKeep(CcdPERIODIC_1s, cancelledKeepHandler, 0);
  CcdCallOnConditionKeep(CcdPERIODIC_1s, survivorKeepHandler, 0);
  int thirdKeep = CcdCallOnConditionKeep(CcdPERIODIC_1s, cancelledKeepHandler, 0);
  CcdCancelCallOnConditionKeep(CcdPERIODIC_1s, firstKeep);
  CcdCancelCallOnConditionKeep(CcdPERIODIC_1s, thirdKeep);
  int target = (CmiMyRank() + 1) % CmiMyNodeSize() + CmiNodeFirst(CmiMyNode());
  CcdCallFnAfterOnPE(remoteAfter, (void *)(size_t)CmiMyPe(), 500, target);

  CpvInitialize(int, exitHandlerId);
  CpvAccess(exitHandlerId) = CmiRegisterHandler(stop_handler);
  CpvInitialize(int, 1sHandlerId);
  CpvAccess(1sHandlerId) = CcdCallOnCondition(CcdPERIODIC_1s, shortHandler, 0);
  CpvInitialize(int, 5sHandlerId);
  CpvAccess(5sHandlerId) = CcdCallOnCondition(CcdPERIODIC_5s, mediumHandler, 0);
  CpvInitialize(int, 10sHandlerId);
  CpvAccess(10sHandlerId) = CcdCallOnCondition(CcdPERIODIC_10s, longHandler, 0);
  CcdCallFnAfter(callAfter7s, 0, 7000);

  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
