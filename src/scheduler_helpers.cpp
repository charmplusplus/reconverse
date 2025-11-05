#include "scheduler.h"

std::vector<QueuePollHandler> g_handlers; //list of handlers
Groups g_groups; //groups of handlers by index

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


