#include "scheduler.h"

std::vector<QueuePollHandler> g_handlers; //list of handlers
Groups g_groups; //groups of handlers by index
CpvDeclare(QueuePollHandlerFn *, poll_handlers);
CpvDeclare(int*, poll_handler_assigned);
//QueuePollHandlerFn *poll_handlers; // fixed size array
#define ARRAY_SIZE 64

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

void add_list_of_handlers(const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers){
    // total frequency
    unsigned int total = 0;
    for(const auto& handler : handlers){
        total += handler.second;
    }
    if(total == 0) return; // nothing to add
    // loop through handlers and add them to the table
    // spread out based on normalized frequency
    CpvInitialize(QueuePollHandlerFn *, poll_handlers);
    CpvAccess(poll_handlers) = new QueuePollHandlerFn[ARRAY_SIZE];
    CpvInitialize(int*, poll_handler_assigned);
    CpvAccess(poll_handler_assigned) = new int[ARRAY_SIZE];
    for(unsigned int i=0; i<ARRAY_SIZE; i++){
        CpvAccess(poll_handler_assigned)[i] = 0;
    }
    //poll_handlers = new QueuePollHandlerFn[ARRAY_SIZE];
    unsigned int current_index = 0; //earliest slot is index within handlers vector
    unsigned int total_assigned = 0;
    int handler_index = 0;//for debugging
    for(const auto& handler : handlers){
        unsigned int freq = handler.second;
        long normalized = lround((freq * ARRAY_SIZE) / static_cast<double>(total)); //estimate of how many slots this handler should take
        CmiPrintf("Handler %d frequency %u normalized to %ld\n", handler_index, freq, normalized);
        if(normalized == 0) normalized = 1; // at least once
        // go through loop and find empty slots
        // spread out as evenly as possible
        unsigned int remaining = normalized;
        unsigned int step = ARRAY_SIZE / normalized;
        unsigned int index = current_index;
        while(remaining > 0){
            //find next empty slot
            if(total_assigned >= ARRAY_SIZE){
                break; // all slots assigned
            }
            while(CpvAccess(poll_handler_assigned)[index] == 0){
                index = (index + 1) % ARRAY_SIZE;
            }
            //CpvAccess(poll_handlers)[index] = handler.first;
            CpvAccess(poll_handler_assigned)[index] = 1;
            //poll_handlers[index] = handler.first;
            total_assigned++;
            CmiPrintf("Adding handler %d at index %d\n", handler_index, index);
            remaining--;
            index = (index + step) % ARRAY_SIZE;
        }
        current_index = (current_index + 1) % ARRAY_SIZE;
        handler_index++;
    }
}
