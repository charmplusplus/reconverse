#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include <thread>
#include <array>
#include <string>
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

// Number of complete trips around the polling table between frequency
// adjustments.  One trip is SCHED_TABLE_SIZE scheduler iterations.  This is
// only the default; +poll_adapt_period <iterations> (or
// RECONVERSE_POLL_ADAPT_PERIOD) overrides it at startup.
#define ADAPT_PERIOD_CYCLES 10
#define ADAPT_PERIOD_DEFAULT ((uint64_t)SCHED_TABLE_SIZE * ADAPT_PERIOD_CYCLES)

using QueuePollHandlerFn = bool(*)(void); //we need a return value to indicate if work was done

struct QueuePollHandler {
    QueuePollHandlerFn fn;
    uint64_t mask{0}; // one bit per slot: bit i == call at loop index i
    unsigned period{0}; // 1..SCHED_TABLE_SIZE, 0 => disabled
    unsigned phase{0};
};

using Groups = std::array<std::vector<QueuePollHandlerFn>, SCHED_TABLE_SIZE>;

// Per-PE polling table.
//
// The table is SCHED_TABLE_SIZE slots; the scheduler walks it and calls the
// handler in each slot.  A queue polled from more slots is polled more often,
// so the number of slots a queue holds *is* its polling frequency.
//
// Each PE counts how many messages it pulled from each queue (a poll handler
// returning true means it dequeued and handled one message).  Every
// ADAPT_PERIOD_CYCLES trips around the table those counters are turned back
// into slot counts, so queues that are actually producing work get polled more
// often.  Every registered queue keeps at least one slot, so nothing can be
// starved out of the table entirely.
struct PollTable {
    std::vector<QueuePollHandlerFn> fns;   // registered queue pollers
    std::vector<std::string> names;        // for reporting
    std::vector<unsigned> baseFreq;        // relative frequency at registration
    std::vector<uint64_t> counts;          // messages pulled since last adjust
    std::vector<uint64_t> polls;           // times polled since last adjust
    std::vector<uint64_t> lifetime;        // messages pulled since startup
    std::vector<unsigned> slotsOf;         // slots currently held
    QueuePollHandlerFn slots[SCHED_TABLE_SIZE]; // the table the scheduler walks
    int owner[SCHED_TABLE_SIZE];           // handler index per slot, -1 = filler
    uint64_t adjustments{0};               // how many times we have re-balanced
    uint64_t adaptPeriod{ADAPT_PERIOD_DEFAULT}; // scheduler iterations per adjust
    uint64_t redraws{0};                   // adjustments that changed the slot counts
    bool adaptive{true};
    bool report{false};                    // dump the table when the scheduler exits
    // +poll_adapt_sample: every 10 s write this PE's slots and the work each
    // handler did since the previous sample (POLLSAMPLE lines, on stderr), so
    // the table can be followed through a run instead of only at exit.
    bool sample{false};
    std::vector<uint64_t> sampledLifetime;  // lifetime counts at last sample

    // Adjustment controls.  The defaults reproduce the unsmoothed rule
    // exactly: each window's measurement becomes the next table.
    //   skipIdle   : a sweep that found nothing probed every slot and charged
    //                each queue once per slot it holds; take those polls back
    //                so all-idle sweeps do not enter the hit-rate denominator.
    //   alpha      : EWMA weight on the new measurement, over queue shares.
    //                1 = no smoothing.
    //   hysteresis : leave the table alone unless some queue's slot count
    //                would change by more than this many slots.
    //   maxMove    : at most this many slots change owner per adjustment
    //                (0 = unlimited).
    bool skipIdle{false};
    double alpha{1.0};
    unsigned hysteresis{0};
    unsigned maxMove{0};
    std::vector<double> share;             // smoothed queue shares, sum 1
    // How the counters become weights.  See pollTableAdapt.
    //   COUNT   : weight = messages pulled.  This is the specified rule and
    //             the default.
    //   HITRATE : weight = messages pulled / times polled.  Tried as a check
    //             on the instability COUNT shows; it does not remove it.
    enum AdaptMode { ADAPT_COUNT, ADAPT_HITRATE } mode{ADAPT_COUNT};
};

CpvExtern(PollTable *, poll_table);

void add_handler(QueuePollHandlerFn fn, unsigned period, unsigned phase = 0);

// Add multiple handlers at once
// pairs of poll handlers and relative frequencies (will be normalized regardless of actual value)
// (frequency/total)*SCHED_TABLE_SIZE
// example: if the frequencies are 8, 1, 16, 1, 4, then they are added up to 30, then normalized to 17, 2, 34, 2, 9
// then assign to slots based on these normalized values
void add_list_of_handlers(const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers);

// Same, but naming each handler so the adaptation can be reported.
void add_list_of_handlers(
    const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers,
    const std::vector<std::string>& names, char **argv);

// Re-balance this PE's table from its message counters.  Called by the
// scheduler every pt->adaptPeriod iterations; exposed for tests.
void pollTableAdapt(PollTable *pt);

// Lay out `weights` over the table, giving every handler at least one slot and
// spreading each handler's slots as evenly as possible.
void pollTableAssign(PollTable *pt, const std::vector<uint64_t>& weights);

// Lay out exact per-handler slot counts (summing to at most SCHED_TABLE_SIZE).
void pollTableLayout(PollTable *pt, const std::vector<unsigned>& slots);

// Print this PE's table if +poll_adapt_report was given; called from
// ConverseExit.
void CmiPollingReportAtExit(void);

void CsdSchedulerRegistered();
void CsdSchedulePollRegistered();

// ---------------------------------------------------------------------------
// Original scheduler (+old-scheduler)
// ---------------------------------------------------------------------------

void CsdSchedulerOld();
void CsdSchedulePollOld();

void CsdScheduler();
#endif
