#include "scheduler.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

std::vector<QueuePollHandler> g_handlers; //list of handlers
Groups g_groups; //groups of handlers by index
CpvDeclare(QueuePollHandlerFn *, poll_handlers);
CpvDeclare(int*, poll_handler_assigned);
CpvDeclare(PollTable *, poll_table);

// default handler used to safely occupy any unassigned slot
static bool pollNoWork() { return false; }

static void pollTableSample(void *);  // +poll_adapt_sample, defined below

// Build a slot mask for a period n (1..SCHED_TABLE_SIZE) with optional phase (0..n-1)
inline uint64_t make_mask_every_n(unsigned n, unsigned phase = 0) {
    if (n == 0) return 0ULL;
    if (n == 1) return SCHED_ALL_SLOTS;
    if (n > SCHED_TABLE_SIZE) n = SCHED_TABLE_SIZE; // clamp to the table
    uint64_t mask = 0ULL;
    for (unsigned pos = 0; pos < SCHED_TABLE_SIZE; ++pos) {
        if (((pos + phase) % n) == 0) mask |= (1ULL << pos);
    }
    return mask;
}

// Rebuild groups from current handler masks (in-place).
// Single-threaded callers may call this whenever a handler mask changes.
inline void rebuild_groups() {
    // Clear all groups
    for (auto &v : g_groups) v.clear();

    // Populate groups from each handler's mask
    for (const auto &h : g_handlers) {
        uint64_t m = h.mask;
        if (m == 0) continue;
        for (unsigned bit = 0; bit < SCHED_TABLE_SIZE; ++bit) {
            if ((m >> bit) & 1ULL) {
                g_groups[bit].push_back(h.fn);
            }
        }
    }
}

// Set handler period and phase (period: 1..SCHED_TABLE_SIZE, 0 disables).
// Rebuilds groups immediately (cheap relative to hot path).
inline void set_frequency(size_t handlerIndex, unsigned period, unsigned phase = 0) {
    if (handlerIndex >= g_handlers.size()) return;
    QueuePollHandler &h = g_handlers[handlerIndex];

    if (period == 0) {
        h.period = 0;
        h.phase = 0;
        h.mask = 0ULL;
    } else {
        if (period > SCHED_TABLE_SIZE) period = SCHED_TABLE_SIZE;
        h.period = period;
        h.phase = phase % period;
        h.mask = make_mask_every_n(h.period, h.phase);
    }
    rebuild_groups();
}

// Add a handler that will poll a queue at given frequency.
void add_handler(QueuePollHandlerFn fn, unsigned period, unsigned phase)
{
    g_handlers.push_back({fn});
    size_t index = g_handlers.size() - 1;
    set_frequency(index, period, phase);
}

// ---------------------------------------------------------------------------
// Slot allocation
//
// Turn a set of relative weights into slot counts over the SCHED_TABLE_SIZE-entry
// table, then lay each handler's slots out as evenly as the table allows.
//
// Every registered handler is guaranteed at least one slot: we hand out one
// slot each first, and only then distribute what is left in proportion to the
// weights (largest-remainder, so the totals come out exact).  A queue that has
// produced nothing therefore keeps a foothold and can recover when its traffic
// comes back -- without that, a queue that went quiet could never be polled
// again and would be starved permanently.
// ---------------------------------------------------------------------------
static std::vector<unsigned> pollTableApportion(
    unsigned n, const std::vector<uint64_t>& weights) {
    std::vector<unsigned> slots(n, 0);

    if (n >= SCHED_TABLE_SIZE) {
        // More queues than slots: everyone gets one, extras are dropped.
        for (unsigned i = 0; i < n && i < SCHED_TABLE_SIZE; ++i) slots[i] = 1;
    } else {
        for (unsigned i = 0; i < n; ++i) slots[i] = 1;      // the floor
        unsigned remaining = SCHED_TABLE_SIZE - n;

        uint64_t total = 0;
        for (uint64_t w : weights) total += w;

        if (total == 0) {
            // Nothing observed at all: spread the remainder evenly so the
            // table stays neutral rather than collapsing onto handler 0.
            for (unsigned i = 0; i < remaining; ++i) slots[i % n]++;
        } else {
            // Largest-remainder apportionment of `remaining`.
            std::vector<double> exact(n);
            unsigned handed = 0;
            for (unsigned i = 0; i < n; ++i) {
                exact[i] = (double)weights[i] * (double)remaining / (double)total;
                unsigned whole = (unsigned)exact[i];
                slots[i] += whole;
                handed += whole;
                exact[i] -= whole;              // keep the fractional part
            }
            // Hand out the leftovers to the largest fractions.
            std::vector<unsigned> order(n);
            for (unsigned i = 0; i < n; ++i) order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](unsigned a, unsigned b) { return exact[a] > exact[b]; });
            for (unsigned k = 0; handed < remaining; ++k, ++handed) {
                slots[order[k % n]]++;
            }
        }
    }

    return slots;
}

void pollTableAssign(PollTable *pt, const std::vector<uint64_t>& weights) {
    const unsigned n = static_cast<unsigned>(pt->fns.size());
    if (n == 0) return;
    pollTableLayout(pt, pollTableApportion(n, weights));
}

void pollTableLayout(PollTable *pt, const std::vector<unsigned>& slots) {
    const unsigned n = static_cast<unsigned>(pt->fns.size());
    if (n == 0) return;

    // Lay the slots out.  For a handler holding s slots the ideal positions are
    // evenly spaced at (j + 0.5) * SCHED_TABLE_SIZE / s; place each at the nearest
    // free slot, searching outward.  Handlers with the most slots go first so
    // the frequently-polled queues get the even spacing, and the sparse ones
    // fill the gaps.
    for (unsigned i = 0; i < SCHED_TABLE_SIZE; ++i) {
        pt->slots[i] = pollNoWork;
        pt->owner[i] = -1;
    }

    std::vector<unsigned> order(n);
    for (unsigned i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](unsigned a, unsigned b) { return slots[a] > slots[b]; });

    for (unsigned oi = 0; oi < n; ++oi) {
        const unsigned h = order[oi];
        const unsigned s = slots[h];
        if (s == 0) continue;
        for (unsigned j = 0; j < s; ++j) {
            unsigned ideal =
                (unsigned)(((double)j + 0.5) * (double)SCHED_TABLE_SIZE / (double)s);
            if (ideal >= SCHED_TABLE_SIZE) ideal = SCHED_TABLE_SIZE - 1;
            // nearest free slot, searching outward from `ideal`
            unsigned placed = SCHED_TABLE_SIZE;
            for (unsigned d = 0; d < SCHED_TABLE_SIZE; ++d) {
                unsigned up = (ideal + d) % SCHED_TABLE_SIZE;
                if (pt->owner[up] < 0) { placed = up; break; }
                unsigned dn = (ideal + SCHED_TABLE_SIZE - d) % SCHED_TABLE_SIZE;
                if (pt->owner[dn] < 0) { placed = dn; break; }
            }
            if (placed == SCHED_TABLE_SIZE) break; // table full
            pt->slots[placed] = pt->fns[h];
            pt->owner[placed] = (int)h;
        }
    }

    pt->slotsOf.assign(n, 0);
    for (unsigned i = 0; i < SCHED_TABLE_SIZE; ++i) {
        if (pt->owner[i] >= 0) pt->slotsOf[pt->owner[i]]++;
    }

    // Keep the legacy flat view in sync for any code still reading it.
    if (CpvAccess(poll_handlers)) {
        for (unsigned i = 0; i < SCHED_TABLE_SIZE; ++i) {
            CpvAccess(poll_handlers)[i] = pt->slots[i];
            CpvAccess(poll_handler_assigned)[i] = (pt->owner[i] >= 0) ? 1 : 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Adaptation
//
// Re-apportion the table from the per-queue message counters collected since
// the last adjustment, then clear them for the next window.
// ---------------------------------------------------------------------------
// Round non-negative slot targets that sum to SCHED_TABLE_SIZE to integers with the
// same sum (largest remainder).  Targets >= 1 stay >= 1.
static std::vector<unsigned> roundSlots(const std::vector<double>& target) {
    const size_t n = target.size();
    std::vector<unsigned> slots(n);
    std::vector<double> frac(n);
    unsigned handed = 0;
    for (size_t i = 0; i < n; ++i) {
        slots[i] = (unsigned)target[i];
        frac[i] = target[i] - slots[i];
        handed += slots[i];
    }
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return frac[a] > frac[b]; });
    for (size_t k = 0; handed < SCHED_TABLE_SIZE && n; ++k, ++handed) slots[order[k % n]]++;
    return slots;
}

// Smoothed adjustment: EWMA over queue shares, a cap on slots moved per
// adjustment, and hysteresis on redraws.
static void pollTableAdaptTuned(PollTable *pt, const std::vector<uint64_t>& weights) {
    const size_t n = pt->fns.size();
    if (n >= SCHED_TABLE_SIZE) return;

    double wsum = 0;
    for (size_t i = 0; i < n; ++i) wsum += (double)weights[i];
    if (wsum <= 0) return;

    // 1. EWMA of the measured shares.  The state keeps evolving even when the
    //    table is not redrawn, so a small persistent shift eventually shows.
    for (size_t i = 0; i < n; ++i)
        pt->share[i] = pt->alpha * ((double)weights[i] / wsum) +
                       (1.0 - pt->alpha) * pt->share[i];

    // 2. Apportion the free slots after the one-slot floor, as
    //    pollTableAssign does.
    std::vector<double> target(n);
    const double freeSlots = (double)(SCHED_TABLE_SIZE - n);
    for (size_t i = 0; i < n; ++i) target[i] = 1.0 + pt->share[i] * freeSlots;
    std::vector<unsigned> proposed = roundSlots(target);

    // 3. Limit how many slots change owner in one adjustment by moving only
    //    part of the way from the current counts toward the proposal.
    const std::vector<unsigned>& cur = pt->slotsOf;
    if (pt->maxMove > 0) {
        unsigned moved = 0;
        for (size_t i = 0; i < n; ++i)
            if (proposed[i] > cur[i]) moved += proposed[i] - cur[i];
        if (moved > pt->maxMove) {
            const double lambda = (double)pt->maxMove / moved;
            for (size_t i = 0; i < n; ++i)
                target[i] = cur[i] + lambda * ((double)proposed[i] - cur[i]);
            proposed = roundSlots(target);
        }
    }

    // 4. Hysteresis: skip the redraw unless some queue moves by more than
    //    `hysteresis` slots.  An unchanged table is always left as it is.
    unsigned maxDelta = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned d = proposed[i] > cur[i] ? proposed[i] - cur[i] : cur[i] - proposed[i];
        maxDelta = std::max(maxDelta, d);
    }
    if (maxDelta == 0 || maxDelta <= pt->hysteresis) return;

    pollTableLayout(pt, proposed);
    pt->redraws++;
}

void pollTableAdapt(PollTable *pt) {
    if (!pt || pt->fns.empty()) return;

    bool any = false;
    for (uint64_t c : pt->counts) {
        if (c) { any = true; break; }
    }
    // A completely idle window carries no information about what the mix
    // should be, so leave the table as it is rather than flattening it.
    if (!any) return;

    std::vector<uint64_t> weights(pt->fns.size());

    if (pt->mode == PollTable::ADAPT_COUNT) {
        // The literal rule: slots in proportion to messages pulled.
        //
        // Note this is unstable for a queue that is rarely empty.  Messages
        // pulled is throughput *achieved*, and a queue can only be drained as
        // often as it is polled, so counts ~= slots for any busy queue.  Slots
        // then feed back into counts and any extreme split is a fixed point:
        // in practice the table collapses to one queue holding 61 of 64 slots
        // and stays there even when the traffic is evenly mixed.
        weights = pt->counts;
    } else {
        // Hit rate: of the times we polled this queue, how often did it have
        // something?  A saturated queue scores ~1.0 however many slots it
        // holds, so the signal does not depend on the current allocation and
        // the feedback loop above disappears.  A queue that is usually empty
        // scores low and gives its slots up.
        for (size_t i = 0; i < pt->fns.size(); ++i) {
            uint64_t polls = pt->polls[i] ? pt->polls[i] : 1;
            weights[i] = (pt->counts[i] * 1000ULL) / polls;
        }
    }

    const bool tuned = pt->alpha < 1.0 || pt->hysteresis > 0 || pt->maxMove > 0;
    if (!tuned) {
        const std::vector<unsigned> before = pt->slotsOf;
        pollTableAssign(pt, weights);
        if (pt->slotsOf != before) pt->redraws++;
    } else {
        pollTableAdaptTuned(pt, weights);
    }
    pt->adjustments++;
    std::fill(pt->counts.begin(), pt->counts.end(), 0);
    std::fill(pt->polls.begin(), pt->polls.end(), 0);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static void add_list_of_handlers_impl(
    const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers,
    const std::vector<std::string>& names, char **argv)
{
    unsigned int total = 0;
    for (const auto& handler : handlers) total += handler.second;
    if (total == 0) return; // nothing to add

    CpvInitialize(QueuePollHandlerFn *, poll_handlers);
    CpvAccess(poll_handlers) = new QueuePollHandlerFn[SCHED_TABLE_SIZE];
    CpvInitialize(int*, poll_handler_assigned);
    CpvAccess(poll_handler_assigned) = new int[SCHED_TABLE_SIZE];
    for (unsigned int i = 0; i < SCHED_TABLE_SIZE; i++) {
        CpvAccess(poll_handler_assigned)[i] = 0;
        CpvAccess(poll_handlers)[i] = pollNoWork;
    }

    CpvInitialize(PollTable *, poll_table);
    PollTable *pt = new PollTable();
    CpvAccess(poll_table) = pt;

    for (size_t i = 0; i < handlers.size(); ++i) {
        pt->fns.push_back(handlers[i].first);
        pt->baseFreq.push_back(handlers[i].second);
        pt->names.push_back(i < names.size() ? names[i]
                                            : ("queue" + std::to_string(i)));
    }
    pt->counts.assign(pt->fns.size(), 0);
    pt->polls.assign(pt->fns.size(), 0);
    pt->lifetime.assign(pt->fns.size(), 0);

    // Adaptation is on by default on this branch; +no_adaptive_polling pins the
    // table to the registered frequencies so the two can be compared.
    pt->adaptive = true;
    if (argv && CmiGetArgFlag(argv, "+no_adaptive_polling")) pt->adaptive = false;
    const char *env = getenv("RECONVERSE_ADAPTIVE_POLLING");
    if (env && (env[0] == '0')) pt->adaptive = false;

    /* Weighting rule: count unless +poll_adapt_mode says otherwise; see
       pollTableAdapt. */
    char *modeStr = NULL;
    if (argv) CmiGetArgString(argv, "+poll_adapt_mode", &modeStr);
    if (!modeStr) {
        char *e = getenv("RECONVERSE_POLL_ADAPT_MODE");
        modeStr = e;
    }
    if (modeStr && strcmp(modeStr, "count") == 0)
        pt->mode = PollTable::ADAPT_COUNT;
    else if (modeStr && strcmp(modeStr, "hitrate") == 0)
        pt->mode = PollTable::ADAPT_HITRATE;

    // Adjustment period in scheduler iterations.  The default is ten trips
    // around the table; larger values adapt less often from longer windows.
    CmiInt8 period = 0;
    if (!(argv && CmiGetArgLong(argv, "+poll_adapt_period", &period))) {
        const char *e = getenv("RECONVERSE_POLL_ADAPT_PERIOD");
        if (e) period = strtoll(e, NULL, 10);
    }
    if (period > 0) pt->adaptPeriod = (uint64_t)period;

    // Adjustment controls; see PollTable.  Flags override the environment.
    const char *e;
    if ((e = getenv("RECONVERSE_POLL_ADAPT_ALPHA")) && *e) pt->alpha = strtod(e, NULL);
    int hyst = 0, maxMove = 0;
    if ((e = getenv("RECONVERSE_POLL_ADAPT_HYSTERESIS")) && *e) hyst = atoi(e);
    if ((e = getenv("RECONVERSE_POLL_ADAPT_MAX_MOVE")) && *e) maxMove = atoi(e);
    if ((e = getenv("RECONVERSE_POLL_ADAPT_SKIP_IDLE")) && e[0] == '1') pt->skipIdle = true;
    if (argv) {
        CmiGetArgDouble(argv, "+poll_adapt_alpha", &pt->alpha);
        CmiGetArgInt(argv, "+poll_adapt_hysteresis", &hyst);
        CmiGetArgInt(argv, "+poll_adapt_max_move", &maxMove);
        if (CmiGetArgFlag(argv, "+poll_adapt_skip_idle")) pt->skipIdle = true;
    }
    pt->alpha = std::min(1.0, std::max(0.0, pt->alpha));
    pt->hysteresis = hyst > 0 ? (unsigned)hyst : 0;
    pt->maxMove = maxMove > 0 ? (unsigned)maxMove : 0;

    if (argv && CmiGetArgFlag(argv, "+poll_adapt_report")) pt->report = true;

    if (argv && CmiGetArgFlag(argv, "+poll_adapt_sample")) pt->sample = true;
    const char *ps = getenv("RECONVERSE_POLL_ADAPT_SAMPLE");
    if (ps && ps[0] == '1') pt->sample = true;
    const char *rep = getenv("RECONVERSE_POLL_ADAPT_REPORT");
    if (rep && rep[0] == '1') pt->report = true;

    // Seed the table from the frequencies given at registration.
    std::vector<uint64_t> seed(pt->fns.size());
    for (size_t i = 0; i < pt->fns.size(); ++i) seed[i] = pt->baseFreq[i];
    pollTableAssign(pt, seed);

    // The smoothed shares start at the registered frequencies.
    double bsum = 0;
    for (unsigned f : pt->baseFreq) bsum += f;
    pt->share.assign(pt->fns.size(), 0.0);
    for (size_t i = 0; i < pt->fns.size(); ++i)
        pt->share[i] = bsum > 0 ? pt->baseFreq[i] / bsum : 0.0;

    if (pt->sample) {
        pt->sampledLifetime.assign(pt->fns.size(), 0);
        if (CmiMyPe() == 0) {
            std::string header = "POLLSAMPLE_HEADER,pe,wall,adj,redraws";
            for (const auto &name : pt->names) header += ",slots:" + name;
            for (const auto &name : pt->names) header += ",work:" + name;
            header += "\n";
            (void)!write(STDERR_FILENO, header.data(), header.size());
        }
        CcdCallOnConditionKeep(CcdPERIODIC_10s, pollTableSample, nullptr);
    }
}

// CcdPERIODIC_10s callback registered by +poll_adapt_sample.  Each line goes
// out in a single write() to stderr: stdout is buffered and flushed at
// arbitrary byte boundaries, so lines from different PEs and processes could
// be cut and interleaved, and applications print partial lines there too.
static void pollTableSample(void *) {
    PollTable *pt = CpvAccess(poll_table);
    if (!pt) return;
    const size_t n = pt->fns.size();
    if (pt->sampledLifetime.size() != n) pt->sampledLifetime.assign(n, 0);
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "POLLSAMPLE,%d,%.3f,%lld,%lld", CmiMyPe(),
                       CmiWallTimer(), (long long)pt->adjustments,
                       (long long)pt->redraws);
    for (size_t i = 0; i < n && off < (int)sizeof(buf) - 32; ++i)
        off += snprintf(buf + off, sizeof(buf) - off, ",%u", pt->slotsOf[i]);
    for (size_t i = 0; i < n && off < (int)sizeof(buf) - 32; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off, ",%llu",
                        (unsigned long long)(pt->lifetime[i] - pt->sampledLifetime[i]));
        pt->sampledLifetime[i] = pt->lifetime[i];
    }
    off += snprintf(buf + off, sizeof(buf) - off, "\n");
    (void)!write(STDERR_FILENO, buf, std::min(off, (int)sizeof(buf) - 1));
}

void add_list_of_handlers(
    const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers)
{
    add_list_of_handlers_impl(handlers, {}, nullptr);
}

void add_list_of_handlers(
    const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers,
    const std::vector<std::string>& names, char **argv)
{
    add_list_of_handlers_impl(handlers, names, argv);
}

// ---------------------------------------------------------------------------
// Reporting, for benchmarks and debugging
// ---------------------------------------------------------------------------
// Under +old-scheduler no table is ever built, so these report nothing.
static PollTable *myPollTable() {
    return CpvInitialized(poll_table) ? CpvAccess(poll_table) : nullptr;
}

extern "C" int CmiPollingNumQueues(void) {
    PollTable *pt = myPollTable();
    return pt ? (int)pt->fns.size() : 0;
}

extern "C" int CmiPollingSlots(int i) {
    PollTable *pt = myPollTable();
    if (!pt || i < 0 || i >= (int)pt->slotsOf.size()) return 0;
    return (int)pt->slotsOf[i];
}

extern "C" const char *CmiPollingName(int i) {
    PollTable *pt = myPollTable();
    if (!pt || i < 0 || i >= (int)pt->names.size()) return "?";
    return pt->names[i].c_str();
}

extern "C" long long CmiPollingCount(int i) {
    PollTable *pt = myPollTable();
    if (!pt || i < 0 || i >= (int)pt->lifetime.size()) return 0;
    return (long long)pt->lifetime[i];
}

extern "C" long long CmiPollingAdjustments(void) {
    PollTable *pt = myPollTable();
    return pt ? (long long)pt->adjustments : 0;
}

extern "C" int CmiPollingAdaptive(void) {
    PollTable *pt = myPollTable();
    return (pt && pt->adaptive) ? 1 : 0;
}

extern "C" void CmiPollingDump(const char *tag) {
    PollTable *pt = myPollTable();
    if (!pt) return;
    char buf[512];
    int off = snprintf(buf, sizeof(buf), "[PE %d] %s slots:", CmiMyPe(),
                       tag ? tag : "polling");
    for (size_t i = 0; i < pt->fns.size() && off < (int)sizeof(buf) - 64; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off, " %s=%u(%lld)",
                        pt->names[i].c_str(), pt->slotsOf[i],
                        (long long)pt->lifetime[i]);
    }
    snprintf(buf + off, sizeof(buf) - off, " redraw=%lld adj=%lld\n",
             (long long)pt->redraws, (long long)pt->adjustments);
    CmiPrintf("%s", buf);
}

// Called by every PE from ConverseExit.  Charm++ reaches ConverseExit from a
// message handler, without returning from CsdScheduler, so this is the one
// exit point common to Converse and Charm++ programs.
void CmiPollingReportAtExit(void) {
    PollTable *pt = myPollTable();
    if (!pt || !pt->report) return;
    pt->report = false;
    CmiPollingDump("final");
}
