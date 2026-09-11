// handler: register many handlers; the test passes if registration and the
// ack round trip work with a crowded handler table.
#include "megarecon.h"

static void handler_dummy(void *msg) { CmiFree(msg); }

void handler_init(void) { megarecon_ack(); }

void handler_moduleinit(void) {
  for (int i = 0; i < 300; i++)
    CmiRegisterHandler(handler_dummy);
}
