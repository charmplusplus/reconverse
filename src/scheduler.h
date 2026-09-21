#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include <thread>
#include <array>
#include <vector>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// Scheduler selection
//
// Two scheduler implementations live side by side:
//
//   scheduler_registered.cpp  the default. Queues register poll handlers into
//                             a slot table (see scheduler_registry.cpp) and the
//                             loop sweeps that table, so adding a queue no
//                             longer means editing the loop.
//
//   scheduler_old.cpp         the original hardcoded if/else chain over a fixed
//                             set of queues, kept reachable with the
//                             +old-scheduler runtime flag.
//
// The public CsdScheduler()/CsdSchedulePoll() in scheduler.cpp dispatch to one
// of the two. Queue registration is the default; +old-scheduler opts out.
// ---------------------------------------------------------------------------

// Set once by CmiSchedulerInitArgs(), before any PE thread starts, and only
// read afterwards. Prefer CmiSchedulerIsOld() over touching it directly.
extern bool _Cmi_useOldScheduler;
inline bool CmiSchedulerIsOld() { return _Cmi_useOldScheduler; }

// Consumes +old-scheduler from argv. Must run before CmiQueueRegisterInit().
void CmiSchedulerInitArgs(char **argv);

// Idle bookkeeping shared by both implementations.
void CmiSchedulerReleaseIdle();
void CmiSchedulerSetIdle();

// Moves one message a peer process on this host left in the shared-memory
// IPC pool onto the local queue it is bound for. Returns whether it moved one.
// A no-op unless the run was given +ipc (or Charm++ set a pool up). Both
// implementations call it once per loop iteration, ahead of their queues.
bool CmiSchedulerPollIpc();

// ---------------------------------------------------------------------------
// Registration-based scheduler (default)
// ---------------------------------------------------------------------------

// Size of the poll-handler slot table, as one constant everything else is
// derived from. Two properties of the table are load-bearing:
//   - the scheduler loop indexes it with SCHED_TABLE_MASK instead of a modulo,
//     which needs the size to be a power of two;
//   - QueuePollHandler::mask carries one bit per slot, which caps the size at
//     the width of that field.
// Changing the table size means changing SCHED_TABLE_BITS and nothing else.
constexpr unsigned SCHED_TABLE_BITS = 6;
constexpr unsigned SCHED_TABLE_SIZE = 1u << SCHED_TABLE_BITS; // 64 slots
constexpr uint64_t SCHED_TABLE_MASK = SCHED_TABLE_SIZE - 1;   // slot index 0..63
constexpr uint64_t SCHED_ALL_SLOTS =
    ~0ULL >> (8 * sizeof(uint64_t) - SCHED_TABLE_SIZE); // every slot set

static_assert(SCHED_TABLE_SIZE <= 8 * sizeof(uint64_t),
              "QueuePollHandler::mask holds one bit per slot, so the table "
              "cannot exceed 64 slots without widening it");

using QueuePollHandlerFn = bool(*)(void); //we need a return value to indicate if work was done

struct QueuePollHandler {
    QueuePollHandlerFn fn;
    uint64_t mask{0}; // one bit per slot: bit i == call at loop index i
    unsigned period{0}; // 1..SCHED_TABLE_SIZE, 0 => disabled
    unsigned phase{0};
};

using Groups = std::array<std::vector<QueuePollHandlerFn>, SCHED_TABLE_SIZE>;

void add_handler(QueuePollHandlerFn fn, unsigned period, unsigned phase = 0);

// Add multiple handlers at once
// pairs of poll handlers and relative frequencies (will be normalized regardless of actual value)
// (frequency/total)*SCHED_TABLE_SIZE
// example: if the frequencies are 8, 1, 16, 1, 4, then they are added up to 30, then normalized to 17, 2, 34, 2, 9
// then assign to slots based on these normalized values
void add_list_of_handlers(const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers);

void CsdSchedulerRegistered();
void CsdSchedulePollRegistered();

// ---------------------------------------------------------------------------
// Original scheduler (+old-scheduler)
// ---------------------------------------------------------------------------

void CsdSchedulerOld();
void CsdSchedulePollOld();

void CsdScheduler();
#endif
