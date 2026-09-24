// Regression test for the PE thread stack size (ported from
// charmplusplus/charm#3927). Every PE runs a handler that puts a 1 MB frame on
// its own stack and touches every page of it. On PEs with rank != 0 that frame
// lives on a thread created by CmiStartThreads, which on macOS used to get the
// 512 KB default pthread stack -- so this handler faulted with SIGBUS there
// before CmiStartThreads began requesting 8 MB. 1 MB stays well inside the
// 8 MB that macOS main threads and a default Linux RLIMIT_STACK both provide,
// so the test is not itself near any limit.
//
// Each PE acks to PE 0 with the checksum of its frame; PE 0 checks every ack
// and then exits everyone.
#include "converse.h"
#include <cstdio>

struct Msg {
  CmiMessageHeader header;
  int pe;
  unsigned long sum;
};

static const size_t kFrameBytes = 1024 * 1024;
static const size_t kPageBytes = 4096;

CpvDeclare(int, ackIdx);
CpvDeclare(int, exitIdx);
CpvDeclare(int, bigFrameIdx);
CpvDeclare(int, acks);

// The array must really be a stack frame, so this is noinline and the result
// is consumed by the caller through a volatile sink -- otherwise the compiler
// is free to drop the array or move it off the stack entirely.
static volatile unsigned long stack_sink;

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static unsigned long
touch_big_frame(int pe) {
  volatile unsigned char buf[kFrameBytes];
  // Write one byte per page, walking downward from the top of the frame so
  // that an undersized stack faults on the guard page rather than silently
  // scribbling into whatever lies below.
  for (size_t off = kFrameBytes; off > 0; off -= kPageBytes)
    buf[off - 1] = (unsigned char)((off / kPageBytes + pe) & 0xff);
  unsigned long sum = 0;
  for (size_t off = kFrameBytes; off > 0; off -= kPageBytes)
    sum += buf[off - 1];
  stack_sink = sum;
  return sum;
}

static unsigned long expected_sum(int pe) {
  unsigned long sum = 0;
  for (size_t off = kFrameBytes; off > 0; off -= kPageBytes)
    sum += (unsigned char)((off / kPageBytes + pe) & 0xff);
  return sum;
}

static void big_frame_handler(void *vmsg) {
  CmiFree(vmsg);
  unsigned long sum = touch_big_frame(CmiMyPe());
  Msg *a = (Msg *)CmiAlloc(sizeof(Msg));
  a->header.messageSize = sizeof(Msg);
  a->pe = CmiMyPe();
  a->sum = sum;
  CmiSetHandler(a, CpvAccess(ackIdx));
  CmiSyncSendAndFree(0, sizeof(Msg), a);
}

static void ack_handler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  if (m->sum != expected_sum(m->pe))
    CmiAbort("pe_stacksize: PE %d reported checksum %lu, expected %lu", m->pe,
             m->sum, expected_sum(m->pe));
  CmiFree(vmsg);
  if (++CpvAccess(acks) == CmiNumPes()) {
    printf("pe_stacksize: all %d PEs ran a %zu-byte stack frame\n",
           CmiNumPes(), kFrameBytes);
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

static void mymain(int argc, char **argv) {
  CpvInitialize(int, ackIdx);
  CpvInitialize(int, exitIdx);
  CpvInitialize(int, bigFrameIdx);
  CpvInitialize(int, acks);
  CpvAccess(acks) = 0;
  CpvAccess(ackIdx) = CmiRegisterHandler(ack_handler);
  CpvAccess(exitIdx) = CmiRegisterHandler(exit_handler);
  CpvAccess(bigFrameIdx) = CmiRegisterHandler(big_frame_handler);

  Msg *m = (Msg *)CmiAlloc(sizeof(Msg));
  m->header.messageSize = sizeof(Msg);
  CmiSetHandler(m, CpvAccess(bigFrameIdx));
  CmiSyncSendAndFree(CmiMyPe(), sizeof(Msg), m);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
