#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include <thread>
#include <array>
#include <vector>

using QueuePollHandlerFn = bool(*)(void); //we need a return value to indicate if work was done

struct QueuePollHandler {
    QueuePollHandlerFn fn;
    uint64_t mask{0}; // 64-bit mask: bit i == call at loop index i (0..63)
    unsigned period{0}; // 1..64, 0 => disabled
    unsigned phase{0};
};

using Groups = std::array<std::vector<QueuePollHandlerFn>, 64>;

void add_handler(QueuePollHandlerFn fn, unsigned period, unsigned phase = 0);

void CsdScheduler();
#endif
