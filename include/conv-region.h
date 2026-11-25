/*****************************************************************************
  Regions, counters and targets: the Converse layer of the persistent family

  An owner PE registers memory once (a region, optionally divided into
  pieces) and an atomic count with an action (a counter). It gives each origin
  a target: one piece of a region paired with one counter. An origin attaches
  to the target, and each CmiPut through it writes into the piece and
  decrements the counter; the contribution that takes the counter to zero
  queues the counter's action on the owner PE. The owner re-arms the counter
  for the next round.

  * Contiguous host memory only: layouts (strided pieces), device memory,
    streams and variable-size pieces are refused.
  * Origins on the owner's PE, on other PEs of its process, and in other
    processes (through the network, also on the same host).

  The owner's calls (create, add a piece, make a target, re-arm, destroy) run
  on the owner PE; an origin's calls run on any PE. Readiness is the
  application's: an origin puts into a piece only when the program's logic
  says the owner no longer needs its previous contents. See
  doc/internals/persistent-family.md.
 *****************************************************************************/

#ifndef CONV_REGION_H
#define CONV_REGION_H

#include "converse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CMI_MEM_HOST = 0, CMI_MEM_DEVICE = 1 } CmiMemKind;

typedef struct CmiRegionRec *CmiRegion;
typedef struct CmiCounterRec *CmiCounter;
typedef struct CmiAttachmentRec *CmiAttachment;
/* A strided layout of a piece; not defined yet, so pass NULL. */
typedef struct CmiLayout CmiLayout;

/* A target of the whole region rather than of one piece. */
#define CMI_PIECE_ALL (-1)
/* CmiRegionAddPiece flags. A variable-size piece is not implemented yet. */
#define CMI_PIECE_VARSIZE 0x1
/* CmiCounterCreate flags. AUTO_REARM: the runtime re-arms the counter with
   the same N just before its action runs. CHECK_ROUNDS: in an error-checking
   build, an origin's k-th contribution through one attachment aborts unless
   the owner has armed at least k rounds; use it only when each attachment
   contributes once per round. */
#define CMI_COUNTER_AUTO_REARM 0x1
#define CMI_COUNTER_CHECK_ROUNDS 0x2
/* CmiRegionDestroy/CmiCounterDestroy flags: ask every attached process to
   detach, rather than wait for each to call CmiDetachTarget. */
#define CMI_DESTROY_DETACH_ALL 0x1

/* What CmiPut, CmiGet and CmiSignal return. */
#define CMI_REGION_OK 0
#define CMI_REGION_ECAPACITY (-1)    /* past the end of the target's piece */
#define CMI_REGION_ENOREGION (-2)    /* a put or get through a counter-only target */
#define CMI_REGION_ENOCOUNTER (-3)   /* a signal through a target with no counter */
#define CMI_REGION_EUNSUPPORTED (-4) /* a layout or a stream */

/* A counter's action, run on the owner PE from the scheduler each time a
   round completes: a function, a Converse handler, or nothing (the owner
   reads CmiCounterValue). A handler receives a CmiCounterFiredMsg allocated
   for it, and frees it. */
typedef void (*CmiCounterFn)(void *arg, CmiCounter c);
typedef enum {
  CMI_ACTION_NONE = 0,
  CMI_ACTION_FN = 1,
  CMI_ACTION_HANDLER = 2
} CmiCounterActionKind;
typedef struct {
  CmiCounterActionKind kind;
  int handler;
  CmiCounterFn fn;
  void *arg;
} CmiCounterAction;
typedef struct {
  char core[CmiMsgHeaderSizeBytes];
  CmiCounter counter;
  uint32_t round; /* 1 for the counter's first round */
} CmiCounterFiredMsg;

static inline CmiCounterAction CmiCounterActionNone(void) {
  CmiCounterAction a;
  a.kind = CMI_ACTION_NONE;
  a.handler = -1;
  a.fn = NULL;
  a.arg = NULL;
  return a;
}
static inline CmiCounterAction CmiCounterActionFn(CmiCounterFn fn, void *arg) {
  CmiCounterAction a = CmiCounterActionNone();
  a.kind = CMI_ACTION_FN;
  a.fn = fn;
  a.arg = arg;
  return a;
}
static inline CmiCounterAction CmiCounterActionHandler(int handler) {
  CmiCounterAction a = CmiCounterActionNone();
  a.kind = CMI_ACTION_HANDLER;
  a.handler = handler;
  return a;
}

/* Runs on the owner PE, from the scheduler, once a destroy has completed. */
typedef void (*CmiDoneFn)(void *arg);

/* A target: plain data of a fixed size, made by the owner and copied into
   any message. Its fields belong to the runtime. */
#define CMI_TARGET_MAGIC 0x54524d43u /* "CMRT" */
#define CMI_TARGET_RKEY_BYTES 32
#define CMI_TARGET_LAYOUT_BYTES 64
typedef struct CmiTarget {
  uint32_t magic;
  uint16_t flags;
  uint16_t memKind;
  int32_t ownerNode; /* the owner's process, as CmiMyNode() numbers it */
  int32_t ownerPe;   /* owner of the region and of the counter */
  uint32_t regionSlot, regionGen;
  uint32_t counterIndex, counterGen;
  uint16_t sigKind, sigValue; /* how a put with signal names the counter */
  uint32_t reserved0;
  uint64_t pieceAddr;  /* the owner's address of the piece's first byte */
  uint64_t pieceDisp;  /* the same, as an offset from the region's base */
  uint64_t pieceBytes; /* the piece's capacity */
  uint8_t rkey[CMI_TARGET_RKEY_BYTES];       /* the region's remote key */
  uint8_t layout[CMI_TARGET_LAYOUT_BYTES];   /* reserved for strided pieces */
} CmiTarget;
/* The size is part of the interface: targets travel between processes. */
#ifdef __cplusplus
static_assert(sizeof(CmiTarget) == 160, "CmiTarget is 160 bytes");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(CmiTarget) == 160, "CmiTarget is 160 bytes");
#endif

/* Regions (owner PE) */
CmiRegion CmiRegionCreate(void *base, size_t bytes, CmiMemKind kind, int devId,
                          int flags);
void *CmiRegionAlloc(size_t bytes, CmiMemKind kind, int devId, int flags,
                     CmiRegion *out);
/* Returns the piece's number: 0, 1, ... in the order pieces are added. */
int CmiRegionAddPiece(CmiRegion r, size_t offset, size_t bytes,
                      const CmiLayout *layout, int flags);
size_t CmiRegionPieceLength(CmiRegion r, int piece);
/* Completes once every attached process has detached; then done(arg) runs on
   this PE, and memory from CmiRegionAlloc is freed. Memory given to
   CmiRegionCreate may be freed once done has run. done may be NULL. */
void CmiRegionDestroy(CmiRegion r, int flags, CmiDoneFn done, void *arg);

/* Counters (owner PE) */
CmiCounter CmiCounterCreate(int32_t n, CmiCounterAction action, int flags);
/* Arms the next round with n contributions. Legal once the current round's
   action has started (for CMI_ACTION_NONE: once the value is at most 0). If
   n or more of the next round's contributions have already arrived, the
   round fires at once. */
void CmiCounterRearm(CmiCounter c, int32_t n);
int32_t CmiCounterValue(CmiCounter c);
/* Completes once every process attached to it has detached and every region
   it was paired with in CmiTargetMake has been destroyed. */
void CmiCounterDestroy(CmiCounter c, int flags, CmiDoneFn done, void *arg);

/* Targets (owner PE): piece `piece` of r (or CMI_PIECE_ALL) with counter c.
   r may be NULL (a counter alone, for CmiSignal); c may be NULL (puts are not
   counted). r and c must belong to the calling PE. */
CmiTarget CmiTargetMake(CmiRegion r, int piece, CmiCounter c);

/* Origins (any PE). An attachment is one origin's handle on a target; the
   attachments of one process to one region (or counter) share one
   registration with the owner. */
CmiAttachment CmiAttachTarget(const CmiTarget *t);
void CmiDetachTarget(CmiAttachment a);
/* The batch forms send one message per owner process; returns n. */
int CmiAttachTargets(const CmiTarget *t, int n, CmiAttachment *out);
void CmiDetachTargets(CmiAttachment *a, int n);

/* Writes bytes from src at offset within the target's piece, and decrements
   the target's counter, if it has one. local, if not NULL, is a counter of
   this process decremented once src may be reused; a completion that comes
   before the local counter's re-arm is not an error (error-checking builds
   do not check it), and counts toward its next round. srcLayout and stream
   must be NULL. */
int CmiPut(CmiAttachment t, size_t offset, const void *src, size_t bytes,
           const CmiLayout *srcLayout, CmiCounter local, void *stream);
/* Reads bytes at offset within the target's piece into dst; decrements local
   once they are in place. The target's counter is not touched. */
int CmiGet(void *dst, size_t bytes, const CmiLayout *dstLayout,
           CmiAttachment t, size_t offset, CmiCounter local, void *stream);
/* A contribution with no data: decrements the target's counter. */
int CmiSignal(CmiAttachment t);

/* Quiescence detection: fn(created, processed) is called on the PE thread
   where contributions are issued (created) and where rounds are consumed
   (processed); created and processed balance once every round has run.
   Process-wide; NULL removes it. */
typedef void (*CmiRegionQdFn)(int created, int processed);
void CmiRegionSetQdHook(CmiRegionQdFn fn);

#ifdef __cplusplus
}
#endif

#endif /* CONV_REGION_H */
