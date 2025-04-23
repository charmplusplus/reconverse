#ifndef COMMBENCH_H
#define COMMBENCH_H

#include <cstring>
#include "converse.h"

CpvExtern(int, ack_handler);
CpvExtern(char, oversubscribed);

typedef struct EmptyMsg { char core[CmiMsgHeaderSizeBytes]; } EmptyMsg;

#endif