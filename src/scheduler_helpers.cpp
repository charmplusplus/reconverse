#include "scheduler.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

std::vector<QueuePollHandler> g_handlers; //list of handlers
Groups g_groups; //groups of handlers by index
CpvDeclare(QueuePollHandlerFn *, poll_handlers);
CpvDeclare(int*, poll_handler_assigned);
CpvDeclare(PollTable *, poll_table);

// default handler used to safely occupy any unassigned slot
static bool pollNoWork() { return false; }

// Build a 64-bit mask for a period n (1..64) with optional phase (0..n-1)
inline uint64_t make_mask_every_n(unsigned n, unsigned phase = 0) {
    if (n == 0) return 0ULL;
    if (n == 1) return ~0ULL;
    if (n > 64) n = 64; // clamp to 64
    uint64_t mask = 0ULL;
    for (unsigned pos = 0; pos < 64; ++pos) {
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
        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((m >> bit) & 1ULL) {
                g_groups[bit].push_back(h.fn);
            }
        }
    }
}

// Set handler period and phase (period: 1..64, 0 disables).
// Rebuilds groups immediately (cheap relative to hot path).
inline void set_frequency(size_t handlerIndex, unsigned period, unsigned phase = 0) {
    if (handlerIndex >= g_handlers.size()) return;
    QueuePollHandler &h = g_handlers[handlerIndex];

    if (period == 0) {
        h.period = 0;
        h.phase = 0;
        h.mask = 0ULL;
    } else {
        if (period > 64) period = 64;
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
// Turn a set of relative weights into slot counts over the ARRAY_SIZE-entry
// table, then lay each handler's slots out as evenly as the table allows.
//
// Every registered handler is guaranteed at least one slot: we hand out one
// slot each first, and only then distribute what is left in proportion to the
// weights (largest-remainder, so the totals come out exact).  A queue that has
// produced nothing therefore keeps a foothold and can recover when its traffic
// comes back -- without that, a queue that went quiet could never be polled
// again and would be starved permanently.
// ---------------------------------------------------------------------------
void pollTableAssign(PollTable *pt, const std::vector<uint64_t>& weights) {
    const unsigned n = static_cast<unsigned>(pt->fns.size());
    if (n == 0) return;

    std::vector<unsigned> slots(n, 0);

    if (n >= ARRAY_SIZE) {
        // More queues than slots: everyone gets one, extras are dropped.
        for (unsigned i = 0; i < n && i < ARRAY_SIZE; ++i) slots[i] = 1;
    } else {
        for (unsigned i = 0; i < n; ++i) slots[i] = 1;      // the floor
        unsigned remaining = ARRAY_SIZE - n;

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

    // Lay the slots out.  For a handler holding s slots the ideal positions are
    // evenly spaced at (j + 0.5) * ARRAY_SIZE / s; place each at the nearest
    // free slot, searching outward.  Handlers with the most slots go first so
    // the frequently-polled queues get the even spacing, and the sparse ones
    // fill the gaps.
    for (unsigned i = 0; i < ARRAY_SIZE; ++i) {
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
                (unsigned)(((double)j + 0.5) * (double)ARRAY_SIZE / (double)s);
            if (ideal >= ARRAY_SIZE) ideal = ARRAY_SIZE - 1;
            // nearest free slot, searching outward from `ideal`
            unsigned placed = ARRAY_SIZE;
            for (unsigned d = 0; d < ARRAY_SIZE; ++d) {
                unsigned up = (ideal + d) % ARRAY_SIZE;
                if (pt->owner[up] < 0) { placed = up; break; }
                unsigned dn = (ideal + ARRAY_SIZE - d) % ARRAY_SIZE;
                if (pt->owner[dn] < 0) { placed = dn; break; }
            }
            if (placed == ARRAY_SIZE) break; // table full
            pt->slots[placed] = pt->fns[h];
            pt->owner[placed] = (int)h;
        }
    }

    pt->slotsOf.assign(n, 0);
    for (unsigned i = 0; i < ARRAY_SIZE; ++i) {
        if (pt->owner[i] >= 0) pt->slotsOf[pt->owner[i]]++;
    }

    // Keep the legacy flat view in sync for any code still reading it.
    if (CpvAccess(poll_handlers)) {
        for (unsigned i = 0; i < ARRAY_SIZE; ++i) {
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

    pollTableAssign(pt, weights);
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
    CpvAccess(poll_handlers) = new QueuePollHandlerFn[ARRAY_SIZE];
    CpvInitialize(int*, poll_handler_assigned);
    CpvAccess(poll_handler_assigned) = new int[ARRAY_SIZE];
    for (unsigned int i = 0; i < ARRAY_SIZE; i++) {
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

    /* Weighting rule.  hitrate is the default; see pollTableAdapt for why. */
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

    // Seed the table from the frequencies given at registration.
    std::vector<uint64_t> seed(pt->fns.size());
    for (size_t i = 0; i < pt->fns.size(); ++i) seed[i] = pt->baseFreq[i];
    pollTableAssign(pt, seed);
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
extern "C" int CmiPollingNumQueues(void) {
    PollTable *pt = CpvAccess(poll_table);
    return pt ? (int)pt->fns.size() : 0;
}

extern "C" int CmiPollingSlots(int i) {
    PollTable *pt = CpvAccess(poll_table);
    if (!pt || i < 0 || i >= (int)pt->slotsOf.size()) return 0;
    return (int)pt->slotsOf[i];
}

extern "C" const char *CmiPollingName(int i) {
    PollTable *pt = CpvAccess(poll_table);
    if (!pt || i < 0 || i >= (int)pt->names.size()) return "?";
    return pt->names[i].c_str();
}

extern "C" long long CmiPollingCount(int i) {
    PollTable *pt = CpvAccess(poll_table);
    if (!pt || i < 0 || i >= (int)pt->lifetime.size()) return 0;
    return (long long)pt->lifetime[i];
}

extern "C" long long CmiPollingAdjustments(void) {
    PollTable *pt = CpvAccess(poll_table);
    return pt ? (long long)pt->adjustments : 0;
}

extern "C" int CmiPollingAdaptive(void) {
    PollTable *pt = CpvAccess(poll_table);
    return (pt && pt->adaptive) ? 1 : 0;
}

extern "C" void CmiPollingDump(const char *tag) {
    PollTable *pt = CpvAccess(poll_table);
    if (!pt) return;
    char buf[512];
    int off = snprintf(buf, sizeof(buf), "[PE %d] %s slots:", CmiMyPe(),
                       tag ? tag : "polling");
    for (size_t i = 0; i < pt->fns.size() && off < (int)sizeof(buf) - 64; ++i) {
        off += snprintf(buf + off, sizeof(buf) - off, " %s=%u(%lld)",
                        pt->names[i].c_str(), pt->slotsOf[i],
                        (long long)pt->lifetime[i]);
    }
    snprintf(buf + off, sizeof(buf) - off, " adj=%lld\n",
             (long long)pt->adjustments);
    CmiPrintf("%s", buf);
}
