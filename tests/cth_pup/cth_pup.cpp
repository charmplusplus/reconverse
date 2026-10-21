// CthPupThread without an Isomalloc provider, in one process.
//
// Charm++ registers the provider that makes threads migratable between
// processes, and exercises that path from its own tests. This one keeps the
// serializer itself honest when threads.cpp changes and the Charm++ tier does
// not run: on every PE a thread runs to a suspend, is sized, packed and
// unpacked through a CmiPupStream into a new thread, and the new thread is
// resumed. It must come back with its stack (address and contents), its
// stack-offset mapping and its thread-private (Ctv) data intact, and CthSelf
// must name the new thread.
//
// With no provider, CthMigratable() is 0 and CthCreateMigratable falls back
// to a malloc'd stack, which pupping does not copy: the unpacked thread
// resumes on the same stack, which is why this only works within a process.
// The packed-from thread is never awakened or freed, since freeing it would
// free that shared stack; the test leaks its small struct on purpose.
#include "converse.h"
#include <cstring>
#include <vector>

struct PeState {
  CthThread original;
  CthThread revived;
  int *stackAddr;
  int stackValue;
  size_t stackOffset;
  int phase; // 0: not started, 1: suspended, 2: finished after the round trip
};
CpvDeclare(PeState, st);
CtvDeclare(int, marker);

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("cth_pup: %s (PE %d)", what, CmiMyPe());
}

struct Buffer {
  std::vector<char> bytes;
  size_t pos;
};

static void sizeBytes(CmiPupStream *p, void *, size_t n) {
  ((Buffer *)p->user)->pos += n;
}
static void packBytes(CmiPupStream *p, void *data, size_t n) {
  Buffer *b = (Buffer *)p->user;
  memcpy(b->bytes.data() + b->pos, data, n);
  b->pos += n;
}
static void unpackBytes(CmiPupStream *p, void *data, size_t n) {
  Buffer *b = (Buffer *)p->user;
  memcpy(data, b->bytes.data() + b->pos, n);
  b->pos += n;
}

static void body(void *) {
  PeState &s = CpvAccess(st);
  int local = 1000 + CmiMyPe();
  s.stackAddr = &local;
  s.stackValue = local;
  s.stackOffset = CthStackOffset(CthSelf(), (char *)&local);
  check(s.stackOffset != 0, "a local variable is not on the thread's stack");
  CtvAccess(marker) = 2000 + CmiMyPe();
  s.phase = 1;
  CthSuspend();

  // Resumed as the unpacked thread.
  check(CthSelf() == s.revived, "CthSelf is not the unpacked thread");
  check(&local == s.stackAddr, "the stack moved");
  check(local == s.stackValue, "stack contents changed");
  check(CthPointer(CthSelf(), s.stackOffset) == (char *)&local,
        "CthPointer does not invert CthStackOffset on the unpacked thread");
  check(CtvAccess(marker) == 2000 + CmiMyPe(),
        "thread-private data did not survive the round trip");
  s.phase = 2;
}

static void finish(void *, double) {
  check(CpvAccess(st).phase == 2, "the unpacked thread did not finish");
  CmiPrintf("[%d] cth_pup: thread packed, unpacked and resumed; ok\n",
            CmiMyPe());
  CsdExitScheduler();
}

static void roundTrip(void *, double) {
  PeState &s = CpvAccess(st);
  check(s.phase == 1, "the thread did not run to its suspend");

  Buffer b;
  CmiPupStream p;
  p.user = &b;

  b.pos = 0;
  p.mode = CMI_PUP_SIZING;
  p.bytes = sizeBytes;
  check(CthPupThread(&p, s.original) == s.original,
        "sizing returned a different thread");
  const size_t n = b.pos;
  check(n > 0, "sizing measured nothing");
  b.bytes.resize(n);

  b.pos = 0;
  p.mode = CMI_PUP_PACKING;
  p.bytes = packBytes;
  check(CthPupThread(&p, s.original) == s.original,
        "packing returned a different thread");
  check(b.pos == n, "packing wrote a different size than sizing measured");

  b.pos = 0;
  p.mode = CMI_PUP_UNPACKING;
  p.bytes = unpackBytes;
  s.revived = CthPupThread(&p, NULL);
  check(s.revived != NULL && s.revived != s.original,
        "unpacking did not create a new thread");
  check(b.pos == n, "unpacking read a different size than packing wrote");
  check(CthGetIsomallocContext(s.revived).opaque == NULL,
        "an Isomalloc context appeared with no provider registered");

  CthSetStrategyDefault(s.revived);
  CthAwaken(s.revived);
  CcdCallFnAfter(finish, NULL, 200);
}

static void mymain(int argc, char **argv) {
  CpvInitialize(PeState, st);
  CtvInitialize(int, marker);
  PeState &s = CpvAccess(st);
  s.phase = 0;
  s.revived = NULL;

  check(!CthMigratable(), "CthMigratable with no Isomalloc provider");
  CmiIsomallocContext none;
  none.opaque = NULL;
  s.original = CthCreateMigratable(body, NULL, 0, none);
  check(s.original != NULL, "CthCreateMigratable failed");
  CthSetStrategyDefault(s.original);
  CthAwaken(s.original);
  CcdCallFnAfter(roundTrip, NULL, 200);
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
