// Shared declarations for the megarecon test bank. See megarecon.cpp.
#ifndef MEGARECON_H
#define MEGARECON_H
#include "converse.h"

// Report one completion of the running test to the controller on PE 0.
// Callable from any PE, from a handler or from an initiator.
void megarecon_ack(void);

#endif
