#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_
#include "converse.h"
#include "converse_internal.h"
#include "queue.h"
#include <thread>
#include <array>
#include <vector>
#include <cmath>

#define ARRAY_SIZE 64
using QueuePollHandlerFn = bool(*)(void); //we need a return value to indicate if work was done

struct QueuePollHandler {
    QueuePollHandlerFn fn;
    uint64_t mask{0}; // 64-bit mask: bit i == call at loop index i (0..63)
    unsigned period{0}; // 1..64, 0 => disabled
    unsigned phase{0};
};

using Groups = std::array<std::vector<QueuePollHandlerFn>, 64>;

void add_handler(QueuePollHandlerFn fn, unsigned period, unsigned phase = 0);

// Add multiple handlers at once
// pairs of poll handlers and relative frequencies (will be normalized regardless of actual value)
// (frequency/total)*64
// example: if the frequencies are 8, 1, 16, 1, 4, then they are added up to 30, then normalized to 17, 2, 34, 2, 9
// then assign to slots based on these normalized values
void add_list_of_handlers(const std::vector<std::pair<QueuePollHandlerFn, unsigned int>>& handlers);

void CsdScheduler();
#endif
