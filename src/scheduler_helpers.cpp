/* Scheduler table construction and installation. See scheduler.h. */
#include "scheduler.h"
#include <algorithm>
#include <cstring>

CpvDeclare(CsdSchedTable, CsdPollTable);
CpvDeclare(CsdSchedTable, CsdPendingTable);
CpvDeclare(std::vector<CsdSchedTable> *, CsdRetiredTables);
CpvDeclare(int, CsdSchedDepth);

/* same on every PE by registration order; a non-PE thread may install */
static int CsdTableInstallIdx = -1;

/* default entry used to safely occupy any unassigned slot */
static int pollNoWork(void *) { return 0; }

/* the runtime's own queues; defined in scheduler.cpp */
int CsdBuiltinPollEntries(CsdPollEntry *out, int max);

/* Largest-remainder apportionment of 64 slots over the entries' relative
 * frequencies, every entry keeping at least one slot. */
static void allocateSlots(CsdSchedTableStruct *t) {
  const int n = (int)t->fns.size();
  if (n > CSD_TABLE_SLOTS)
    CmiAbort("CsdSchedTableCreate: more than 64 poll entries\n");
  unsigned total = 0;
  for (int i = 0; i < n; i++) total += t->baseFreq[i];
  if (total == 0) total = 1;
  std::vector<double> exact(n);
  std::vector<unsigned> slots(n);
  int assigned = 0;
  for (int i = 0; i < n; i++) {
    exact[i] = (double)t->baseFreq[i] * CSD_TABLE_SLOTS / total;
    slots[i] = std::max(1u, (unsigned)exact[i]);
    assigned += slots[i];
  }
  /* the >=1 floors may overshoot: take slots back from the largest holders */
  while (assigned > CSD_TABLE_SLOTS) {
    int k = 0;
    for (int i = 1; i < n; i++) if (slots[i] > slots[k]) k = i;
    if (slots[k] <= 1) CmiAbort("CsdSchedTableCreate: cannot fit entries in 64 slots\n");
    slots[k]--; assigned--;
  }
  /* hand out the rest by largest fractional remainder */
  while (assigned < CSD_TABLE_SLOTS) {
    int k = -1; double best = -1.0;
    for (int i = 0; i < n; i++) {
      double rem = exact[i] - (double)slots[i];
      if (rem > best) { best = rem; k = i; }
    }
    slots[k]++; assigned++;
  }
  for (int s = 0; s < CSD_TABLE_SLOTS; s++) {
    t->slotFn[s] = pollNoWork; t->slotCtx[s] = nullptr; t->owner[s] = -1;
  }
  /* spread each entry's slots as evenly as possible, in entry order */
  int cursor = 0;
  for (int i = 0; i < n; i++) {
    t->slotsOf[i] = slots[i];
    unsigned remaining = slots[i];
    unsigned step = CSD_TABLE_SLOTS / slots[i];
    int idx = cursor;
    while (remaining > 0) {
      while (t->owner[idx] != -1) idx = (idx + 1) % CSD_TABLE_SLOTS;
      t->slotFn[idx] = t->fns[i]; t->slotCtx[idx] = t->ctxs[i]; t->owner[idx] = i;
      remaining--;
      idx = (idx + step) % CSD_TABLE_SLOTS;
    }
    cursor = (cursor + 1) % CSD_TABLE_SLOTS;
  }
}

CsdSchedTable CsdSchedTableCreate(const CsdPollEntry *entries, int n) {
  return CsdSchedTableCreateEx(entries, n, CSD_BUILTIN_ALL);
}

CsdSchedTable CsdSchedTableCreateEx(const CsdPollEntry *entries, int n, unsigned builtinMask) {
  CsdSchedTableStruct *t = new CsdSchedTableStruct();
  CsdPollEntry all[16];
  CsdPollEntry builtin[16];
  int nall = CsdBuiltinPollEntries(all, 16);
  int nb = 0;
  for (int i = 0; i < nall; i++)
    if (builtinMask & (1u << i)) builtin[nb++] = all[i];
  if (nb == 0) CmiAbort("CsdSchedTableCreateEx: a table must poll at least one runtime queue\n");
  t->numBuiltin = nb;
  auto add = [&](const CsdPollEntry &e) {
    if (e.fn == nullptr) CmiAbort("CsdSchedTableCreate: null poll function\n");
    t->fns.push_back(e.fn); t->ctxs.push_back(e.ctx);
    t->names.push_back(e.name ? e.name : "");
    t->baseFreq.push_back(e.freq == 0 ? 1u : e.freq);
    t->slotsOf.push_back(0); t->counts.push_back(0);
  };
  for (int i = 0; i < nb; i++) add(builtin[i]);
  for (int i = 0; i < n; i++) add(entries[i]);
  allocateSlots(t);
  return t;
}

void CsdSchedTableDestroy(CsdSchedTable t) { delete t; }

int CsdSchedTableSlots(CsdSchedTable t, int entry) {
  int i = t->numBuiltin + entry; /* user entries are numbered from 0 */
  if (i < 0 || i >= (int)t->slotsOf.size()) return -1;
  return (int)t->slotsOf[i];
}

int CsdSchedTableNumBuiltin(CsdSchedTable t) { return t->numBuiltin; }

struct CsdInstallMsg {
  char hdr[CmiMsgHeaderSizeBytes];
  CsdSchedTable table;
};

static void CsdTableInstallHandler(void *vmsg) {
  CsdInstallMsg *m = (CsdInstallMsg *)vmsg;
  /* never swap here: the sweep that dispatched this handler is indexing the
   * current table. Leave it for the loop top. */
  if (CpvAccess(CsdPendingTable) != nullptr)
    CsdSchedTableDestroy(CpvAccess(CsdPendingTable)); /* superseded before use */
  CpvAccess(CsdPendingTable) = m->table;
  CmiFree(m);
}

void CsdSchedTableInstall(int rank, CsdSchedTable t) {
  if (rank == CmiMyRank()) {
    /* Set the pending pointer directly: the caller may free objects the
     * current table polls right after this call, and a message through the
     * self queue would let one more sweep of the old table run first. The
     * swap still happens at the loop top, never inside a sweep. */
    if (CpvAccess(CsdPendingTable) != nullptr) CsdSchedTableDestroy(CpvAccess(CsdPendingTable));
    CpvAccess(CsdPendingTable) = t;
    return;
  }
  CsdInstallMsg *m = (CsdInstallMsg *)CmiAlloc(sizeof(CsdInstallMsg));
  CmiInitMsgHeader(m, (int)sizeof(CsdInstallMsg));
  CmiSetHandler(m, CsdTableInstallIdx);
  m->table = t;
  CmiPushPE(rank, m); /* consumed at the target's loop top */
}

void CsdSchedTableLoopTop(void) {
  CsdSchedTable pending = CpvAccess(CsdPendingTable);
  if (pending != nullptr) {
    CpvAccess(CsdPendingTable) = nullptr;
    CpvAccess(CsdRetiredTables)->push_back(CpvAccess(CsdPollTable));
    CpvAccess(CsdPollTable) = pending;
  }
  /* an outer scheduler loop suspended inside a handler may still be sweeping
   * a retired table: free only when this is the only loop on the PE */
  if (CpvAccess(CsdSchedDepth) == 1) {
    for (CsdSchedTable r : *CpvAccess(CsdRetiredTables)) CsdSchedTableDestroy(r);
    CpvAccess(CsdRetiredTables)->clear();
  }
}

void CsdSchedTableInitPE(void) {
  CpvInitialize(CsdSchedTable, CsdPollTable);
  CpvInitialize(CsdSchedTable, CsdPendingTable);
  CpvInitialize(std::vector<CsdSchedTable> *, CsdRetiredTables);
  CpvInitialize(int, CsdSchedDepth);
  CpvAccess(CsdPendingTable) = nullptr;
  CpvAccess(CsdRetiredTables) = new std::vector<CsdSchedTable>();
  CpvAccess(CsdSchedDepth) = 0;
  int idx = CmiRegisterHandler((CmiHandler)CsdTableInstallHandler);
  if (CsdTableInstallIdx < 0) CsdTableInstallIdx = idx;
  else if (CsdTableInstallIdx != idx)
    CmiAbort("CsdSchedTableInitPE: install handler index differs across PEs\n");
  CpvAccess(CsdPollTable) = CsdSchedTableCreate(nullptr, 0);
}
