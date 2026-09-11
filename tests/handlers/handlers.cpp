// The handler table beyond CmiRegisterHandler: CmiNumberHandler and
// CmiNumberHandlerEx place a handler at a chosen index (Charm++ does this
// for indices agreed across PEs), CmiHandlerToFunction looks one up, and
// the extra header fields CmiSetXHandler / CmiGetXHandler and CmiSetInfo /
// CmiGetInfo, which the seed balancer uses to stash the original handler
// and the info function, round-trip. Each PE runs the checks; PE 0
// collects one ack per PE and exits everyone.
#include "converse.h"
#include <cstdio>

struct Msg {
  CmiMessageHeader header;
  int which;
};

CpvDeclare(int, regIdx);
CpvDeclare(int, regExIdx);
CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);
CpvDeclare(int, acks);
struct Seen {
  int s[4];
};
CpvDeclare(Seen, seen); // per PE: the PEs of a process share this address space
static const int kNumbered = 600; // beyond anything registered so far
static const int kNumberedEx = 601;
static int userTag = 4242;

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("handlers: %s on PE %d", what, CmiMyPe());
}

static void send_ack(void) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  CmiSetHandler(m, CpvAccess(ackIdx));
  CmiSyncSendAndFree(0, sizeof(Msg), m);
}

static void maybe_done(void) {
  for (int i = 0; i < 4; i++)
    if (!CpvAccess(seen).s[i])
      return;
  send_ack();
}

static void h_reg(void *vmsg) {
  CpvAccess(seen).s[0] = 1;
  CmiFree(vmsg);
  maybe_done();
}
static void h_regex(void *vmsg, void *user) {
  check(*(int *)user == userTag, "user pointer of CmiRegisterHandlerEx");
  CpvAccess(seen).s[1] = 1;
  CmiFree(vmsg);
  maybe_done();
}
static void h_numbered(void *vmsg) {
  CpvAccess(seen).s[2] = 1;
  CmiFree(vmsg);
  maybe_done();
}
static void h_numberedex(void *vmsg, void *user) {
  check(*(int *)user == userTag, "user pointer of CmiNumberHandlerEx");
  CpvAccess(seen).s[3] = 1;
  CmiFree(vmsg);
  maybe_done();
}

static void ack_handler(void *vmsg) {
  CmiFree(vmsg);
  if (++CpvAccess(acks) == CmiNumPes()) {
    Msg *e = (Msg *)CmiAlloc(sizeof(Msg));
    e->header.messageSize = sizeof(Msg);
    CmiSetHandler(e, CpvAccess(exitIdx));
    CmiSyncBroadcastAllAndFree(sizeof(Msg), e);
  }
}
static void exit_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

static void self_send(int handler) {
  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  CmiSetHandler(m, handler);
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Msg), m);
}

static void mymain(int argc, char **argv) {
  CpvInitialize(int, regIdx);
  CpvInitialize(int, regExIdx);
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvInitialize(int, acks);
  CpvInitialize(Seen, seen);
  CpvAccess(regIdx) = CmiRegisterHandler(h_reg);
  CpvAccess(regExIdx) = CmiRegisterHandlerEx(h_regex, &userTag);
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  CmiNumberHandler(kNumbered, h_numbered);
  CmiNumberHandlerEx(kNumberedEx, h_numberedex, &userTag);
  check(CmiHandlerToFunction(CpvAccess(regIdx)) == h_reg,
        "CmiHandlerToFunction of a registered handler");
  check(CmiHandlerToFunction(kNumbered) == h_numbered,
        "CmiHandlerToFunction of a numbered handler");

  // header round trips
  Msg probe;
  probe.header.messageSize = sizeof(Msg);
  probe.header.zcMsgType = CMK_REG_NO_ZC_MSG;
  probe.header.nokeep = false;
  CmiSetHandler(&probe, CpvAccess(regIdx));
  CmiSetXHandler(&probe, kNumbered);
  CmiSetInfo(&probe, 77);
  check(CmiGetHandler(&probe) == CpvAccess(regIdx), "CmiGetHandler");
  check(CmiGetXHandler(&probe) == kNumbered, "CmiGetXHandler");
  check(CmiGetInfo(&probe) == 77, "CmiGetInfo");
  check(CmiGetHandlerFunction(&probe) == h_reg, "CmiGetHandlerFunction");

  for (int i = 0; i < 4; i++)
    CpvAccess(seen).s[i] = 0;
  CpvAccess(acks) = 0;
  self_send(CpvAccess(regIdx));
  self_send(CpvAccess(regExIdx));
  self_send(kNumbered);
  self_send(kNumberedEx);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
