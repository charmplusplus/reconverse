#ifndef CPV_MACROS_H
#define CPV_MACROS_H

#include "convcore.h"

// NOTE: these are solely for backwards compatibility
// Do not use in reconverse impl

#define CMK_TAG(x, y) x##y##_

#define CpvDeclare(t, v) t *CMK_TAG(Cpv_, v)
#define CpvStaticDeclare(t, v) static t *CMK_TAG(Cpv_, v)

#define CpvInitialize(t, v)                            \
    do                                                 \
    {                                                  \
        if (false /* I don't understand */)            \
        {                                              \
            CmiNodeBarrier();                          \
        }                                              \
        else                                           \
        {                                              \
            CMK_TAG(Cpv_, v) = new t[CmiMyNodeSize()]; \
            CmiNodeBarrier();                          \
        }                                              \
    } while (0)
;

#define CpvAccess(v) CMK_TAG(Cpv_, v)[CmiMyRank()]

#endif