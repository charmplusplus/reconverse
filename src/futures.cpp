/* Converse futures (Cfuture), the blocking single-assignment cell of the
 * Converse extensions manual.
 *
 * A future resides on the PE that created it: the handle carries that PE
 * and a pointer to the cell. CfutureWait and CfutureDestroy run only on the
 * owning PE; CfutureSet runs anywhere. A remote set sends the value, in a
 * message that is also its storage, to the owning PE, whose handler stores
 * it and awakens the waiting threads. A local set stores it directly. Only
 * the owner ever touches the cell, so no lock is needed even though PEs are
 * threads of one process.
 *
 * Waiters are chained through the Cth "next" pointer, as the Cth standby
 * list does; a thread waits on at most one future at a time.
 */
#include "converse_internal.h"

#include <cstdlib>
#include <cstring>

struct CfutureCell {
  void *value;      // the CfutureValue message once set, else nullptr
  int ready;
  CthThread waiters;
};

struct CfutureValue {
  CmiMessageHeader header;
  CfutureCell *cell; // filled in by CfutureStoreBuffer for a remote set
  int valsize;       // whole message, header included
  double rest[1];    // user data starts here, 8-byte aligned
};

#define void_to_value(v) \
  ((CfutureValue *)((char *)(v) - offsetof(CfutureValue, rest)))

CpvStaticDeclare(int, CfutureStoreIndex);

Cfuture CfutureCreate(void) {
  CfutureCell *cell = (CfutureCell *)malloc(sizeof(CfutureCell));
  _MEMCHECK(cell);
  cell->value = nullptr;
  cell->ready = 0;
  cell->waiters = nullptr;
  Cfuture result;
  result.pe = CmiMyPe();
  result.data = cell;
  return result;
}

static void CfutureAwaken(CfutureCell *cell, CfutureValue *val) {
  cell->value = val;
  cell->ready = 1;
  for (CthThread t = cell->waiters; t; t = CthGetNext(t))
    CthAwaken(t);
  cell->waiters = nullptr;
}

static void CfutureStore(void *msg) {
  CfutureValue *m = (CfutureValue *)msg;
  CfutureAwaken(m->cell, m);
}

void *CfutureCreateBuffer(int bytes) {
  int valsize = sizeof(CfutureValue) + bytes;
  CfutureValue *m = (CfutureValue *)CmiAlloc(valsize);
  CmiSetHandler(m, CpvAccess(CfutureStoreIndex));
  m->valsize = valsize;
  return (void *)(m->rest);
}

void CfutureDestroyBuffer(void *value) { CmiFree(void_to_value(value)); }

void CfutureStoreBuffer(Cfuture f, void *value) {
  CfutureValue *m = void_to_value(value);
  if (f.pe == CmiMyPe()) {
    CfutureAwaken(f.data, m);
  } else {
    m->cell = f.data;
    CmiSyncSendAndFree(f.pe, m->valsize, m);
  }
}

void CfutureSet(Cfuture f, void *value, int len) {
  void *copy = CfutureCreateBuffer(len);
  memcpy(copy, value, len);
  CfutureStoreBuffer(f, copy);
}

void *CfutureWait(Cfuture f) {
  if (f.pe != CmiMyPe())
    CmiAbort("CfutureWait: the future resides on PE %d, not here", f.pe);
  CfutureCell *cell = f.data;
  if (!cell->ready) {
    CthThread self = CthSelf();
    CthSetNext(self, cell->waiters);
    cell->waiters = self;
    CthSuspend();
  }
  return (void *)(((CfutureValue *)cell->value)->rest);
}

void CfutureDestroy(Cfuture f) {
  if (f.pe != CmiMyPe())
    CmiAbort("CfutureDestroy: the future resides on PE %d, not here", f.pe);
  if (f.data->waiters)
    CmiAbort("CfutureDestroy: a thread is still waiting on this future");
  if (f.data->value)
    CmiFree(f.data->value);
  free(f.data);
}

void CfutureModuleInit(void) {
  CpvInitialize(int, CfutureStoreIndex);
  CpvAccess(CfutureStoreIndex) = CmiRegisterHandler(CfutureStore);
}
