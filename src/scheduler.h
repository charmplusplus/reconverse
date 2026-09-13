#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_
/* Per-PE polling table for the scheduler (internal; the public C surface is
 * CsdPollEntry / CsdSchedTable / CsdSchedTableCreate / CsdSchedTableInstall
 * in converse.h).
 *
 * The scheduler walks a 64-slot table and calls the poll function in each
 * slot with its context; a function returns 1 if it dequeued and handled one
 * message. An entry polled from more slots is polled more often, so the
 * number of slots an entry holds IS its relative frequency; within a sweep
 * the first non-empty slot wins, so slot order is a weak priority. A full
 * sweep of all 64 slots precedes any idle declaration.
 *
 * Tables are plain heap objects built for any rank by CsdSchedTableCreate
 * and installed by message (CsdSchedTableInstall); the target PE swaps at
 * the top of its scheduler loop and retires the old table, which is freed
 * only when no scheduler loop on that PE is nested inside a handler that
 * may still be sweeping it. */
#include "converse.h"
#include "converse_internal.h"
#include <cstdint>
#include <string>
#include <vector>

#define CSD_TABLE_SLOTS 64

struct CsdSchedTableStruct {
  std::vector<CsdPollFn> fns;        /* registered entries, in priority order */
  std::vector<void *> ctxs;
  std::vector<std::string> names;
  std::vector<unsigned> baseFreq;    /* relative frequency at registration */
  std::vector<unsigned> slotsOf;     /* slots currently held by each entry */
  std::vector<uint64_t> counts;      /* messages pulled since installation */
  CsdPollFn slotFn[CSD_TABLE_SLOTS]; /* the table the scheduler walks */
  void *slotCtx[CSD_TABLE_SLOTS];
  int owner[CSD_TABLE_SLOTS];        /* entry index per slot, -1 = filler */
  int numBuiltin;                    /* leading entries are the runtime's own queues */
};

CpvExtern(CsdSchedTable, CsdPollTable);    /* the PE's current table */
CpvExtern(CsdSchedTable, CsdPendingTable); /* installed, swapped in at loop top */
CpvExtern(std::vector<CsdSchedTable> *, CsdRetiredTables);
CpvExtern(int, CsdSchedDepth);             /* nesting of scheduler loops on this PE */

/* Called once per PE from converseRunPe: registers the install handler and
 * installs the default table (built-in queues only). */
void CsdSchedTableInitPE(void);
/* Consume a pending install and free retired tables when safe. */
void CsdSchedTableLoopTop(void);

void CsdScheduler();
#endif
