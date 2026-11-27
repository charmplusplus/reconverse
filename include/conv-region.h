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
  says the owner no longer needs its previous contents. A put is not a
  message: it lands whenever it arrives, whatever the owner is doing.

  Checks. Every build aborts on a put, get or signal through a detached
  attachment, through one the owner ended, or through a target whose region
  or counter was destroyed (within the owner's process; from another
  process, the owner catches a transfer that names a destroyed counter or
  region where it lands, in most cases but not all), on a re-arm before the
  round's action has started, and on an owner's call made on another PE. Builds with error checking (the CMake option
  RECONVERSE_ERROR_CHECKING, on by default where CMK_OPTIMIZE is 0) also
  abort on a contribution that arrives after its round fired and before the
  re-arm, and, for CMI_COUNTER_CHECK_ROUNDS counters, on one counted in
  place of another round's. Such an abort means the program's readiness
  logic is wrong there; a message send is the safe choice.

  A round, with one piece per origin and one counter (nOrigins, consume,
  lastRound, startNextStep and sendTarget stand for the application's):

    // owner PE
    static CmiRegion reg;
    static CmiCounter ctr;
    static void onRound(void *arg, CmiCounter c) {
      consume(arg);                   // the last read of this round's data
      if (lastRound()) {              // origins detach after their last put
        CmiRegionDestroy(reg, 0, NULL, NULL);  // memory freed when complete
        CmiCounterDestroy(ctr, 0, NULL, NULL);
      } else {
        CmiCounterRearm(c, nOrigins); // after the last read, before this
        startNextStep();              // PE sends anything that lets an
      }                               // origin put the next round
    }
    void ownerSetup(size_t bytes) {
      void *mem = CmiRegionAlloc(nOrigins * bytes, CMI_MEM_HOST, 0, 0, &reg);
      ctr = CmiCounterCreate(nOrigins, CmiCounterActionFn(onRound, mem), 0);
      for (int o = 0; o < nOrigins; o++) {
        int piece = CmiRegionAddPiece(reg, o * bytes, bytes, NULL, 0);
        CmiTarget t = CmiTargetMake(reg, piece, ctr);
        sendTarget(o, &t);            // plain data, copied into a message
      }
    }

    // origin, on any PE, once its target has arrived
    CmiAttachment a = CmiAttachTarget(&t);
    // each round, when the program's logic says the owner has finished
    // with the previous one; srcFree is a counter of this PE whose action
    // runs once src may be overwritten
    if (CmiPut(a, 0, src, bytes, NULL, srcFree, NULL) != CMI_REGION_OK)
      CmiAbort("CmiPut failed\n");
    // after the last round's put, and its srcFree action
    CmiDetachTarget(a);

  doc/internals/persistent-family.md describes the implementation; the
  design is charm's doc/persistent-family-design.md (section 4.9 has the
  readiness patterns).
 *****************************************************************************/

#ifndef CONV_REGION_H
#define CONV_REGION_H

#include "converse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CmiPut, CmiGet, CmiSignal and CmiAttachTargets return a value that must
   be checked: an error code means nothing was transferred or counted, and
   the owner would wait for that contribution forever. */
#if defined(__GNUC__) || defined(__clang__)
#define CMI_REGION_CHECK_RESULT __attribute__((warn_unused_result))
#else
#define CMI_REGION_CHECK_RESULT
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
/* CmiCounterCreate flags.
   AUTO_REARM: the runtime re-arms the counter with the same N just before
   its action runs, and an explicit CmiCounterRearm of it aborts. Use it
   only when both hold (design 4.2): the action is the last use of the
   round's data (it copies or consumes the data before it returns), and no
   origin can issue the next round's put before the action has run. No
   check catches a violation: a put that lands after the automatic re-arm
   while the action still reads the data counts toward the next round.
   CHECK_ROUNDS: in an error-checking build, an origin's k-th contribution
   through one attachment aborts unless the owner has armed at least k
   rounds. Use it only when each attachment contributes once per round. The
   count starts at the attach: an origin that attaches after round r can run
   r rounds ahead undetected, and a handle attached again starts again from
   zero. Within a process only. */
#define CMI_COUNTER_AUTO_REARM 0x1
#define CMI_COUNTER_CHECK_ROUNDS 0x2
/* CmiRegionDestroy/CmiCounterDestroy flags: ask every attached process to
   detach, rather than wait for each to call CmiDetachTarget. An origin's put,
   get or signal through an attachment the owner ended this way aborts. */
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

/* Regions (owner PE). kind is CMI_MEM_HOST and devId 0; flags is 0. */
CmiRegion CmiRegionCreate(void *base, size_t bytes, CmiMemKind kind, int devId,
                          int flags);
/* Allocates the memory too, and returns it; *out receives the region (out
   must not be NULL). */
void *CmiRegionAlloc(size_t bytes, CmiMemKind kind, int devId, int flags,
                     CmiRegion *out);
/* Returns the piece's number: 0, 1, ... in the order pieces are added.
   layout must be NULL and flags 0 for now. */
int CmiRegionAddPiece(CmiRegion r, size_t offset, size_t bytes,
                      const CmiLayout *layout, int flags);
/* For variable-size pieces, which are not implemented yet: always aborts. */
size_t CmiRegionPieceLength(CmiRegion r, int piece);
/* Completes once every attached process has detached and every contribution
   their detach notices report has landed; then done(arg) runs on this PE,
   and memory from CmiRegionAlloc is freed. Memory given to CmiRegionCreate
   may be freed once done has run. done may be NULL.
   When to call it: once nothing of any origin can still be in flight into
   the region, which the application knows when the region's counter has
   fired a round that every origin's puts counted toward, and no origin puts
   again. Do not destroy on an application message alone ("I am done") in
   two cases, puts without a counter and a counter whose last round has not
   fired: messages are not ordered, so that message can overtake the
   origin's registration, the destroy then completes without waiting for
   that origin, and its puts land in freed memory. Puts without a counter
   have no firing to wait for; give them a counter if the region may be
   destroyed while an origin could still be putting.
   If the region's counter also serves another region still in use, or a
   target with no region, call this only after the counter has fired the last
   round that a put into this region counted toward (the shared-counter
   rule, design 10.2). */
void CmiRegionDestroy(CmiRegion r, int flags, CmiDoneFn done, void *arg);

/* Counters (owner PE). n is the first round's count of contributions. */
CmiCounter CmiCounterCreate(int32_t n, CmiCounterAction action, int flags);
/* Arms the next round with n contributions. Legal once the current round's
   action has started (for CMI_ACTION_NONE: once the value is at most 0);
   earlier, it aborts in every build. Re-arm where the owner has finished
   with the round's data (design 4.2): in the action after the last read, or
   later. A contribution of the next round that arrives before the re-arm
   (an early one) is counted correctly in production builds, and if n of
   them have arrived the round fires at once; but an error-checking build
   aborts on it, since it means the origin did not wait for readiness. With
   two buffers by parity (design 4.9, pattern 2), re-arm a round's counter
   before you send your own next round, or a correct put can arrive early.
   A local counter (CmiPut's or CmiGet's `local`) is not checked: its early
   completions count toward its next round in every build. */
void CmiCounterRearm(CmiCounter c, int32_t n);
/* Contributions still expected in the armed round; 0 or less once it is
   complete (an acquire load). */
int32_t CmiCounterValue(CmiCounter c);
/* Completes once every process attached to it has detached, every
   contribution their notices report has landed, and every region it was
   paired with in CmiTargetMake has been destroyed. Call it after the
   counter has fired the last round that every origin's contributions
   counted toward (see CmiRegionDestroy: an earlier destroy can complete
   before an origin's registration arrives). A counter passed as `local` to
   a CmiPut or CmiGet must not be destroyed until those transfers have
   completed: their completion decrements it. */
void CmiCounterDestroy(CmiCounter c, int flags, CmiDoneFn done, void *arg);

/* Targets (owner PE): piece `piece` of r (or CMI_PIECE_ALL) with counter c.
   r may be NULL (a counter alone, for CmiSignal); c may be NULL (puts are not
   counted). r and c must belong to the calling PE. */
CmiTarget CmiTargetMake(CmiRegion r, int piece, CmiCounter c);

/* Origins (any PE). An attachment is one origin's handle on a target; the
   attachments of one process to one region (or counter) share one
   registration with the owner. */
CmiAttachment CmiAttachTarget(const CmiTarget *t);
/* Ends the attachment. Transfers through it still in flight are allowed:
   the detach notice waits for them to complete locally, and their sources
   are read until then, so keep them unchanged. The handle is invalid
   afterwards: a put, get, signal or detach through it aborts ("detached
   already") until 4096 later detaches in the process have passed, after
   which the handle may be reused for another attachment. */
void CmiDetachTarget(CmiAttachment a);
/* The batch forms send one message per owner process. CmiAttachTargets
   returns n, the number of attachments written to out; an invalid target
   aborts. */
CMI_REGION_CHECK_RESULT int CmiAttachTargets(const CmiTarget *t, int n,
                                             CmiAttachment *out);
void CmiDetachTargets(CmiAttachment *a, int n);

/* Writes bytes from src at offset within the target's piece, and decrements
   the target's counter, if it has one. srcLayout and stream must be NULL.
   Returns CMI_REGION_OK, or an error code and does nothing.
   When src may be reused: local, if not NULL, is a counter of this process
   decremented once src may be overwritten; a completion that comes before
   the local counter's re-arm is not an error, and counts toward its next
   round. Without local: within the owner's process, and to another process
   when the put travels as a message (CmiRegionPutMessageMaxBytes), src may
   be reused when CmiPut returns; a put with signal reads src until its local
   completion, which only local shows. So without local, change src only
   when something else says the put has landed (for example the owner's next
   round of data, design 4.5), or keep a separate buffer per round. */
CMI_REGION_CHECK_RESULT int CmiPut(CmiAttachment a, size_t offset,
                                   const void *src, size_t bytes,
                                   const CmiLayout *srcLayout, CmiCounter local,
                                   void *stream);
/* Reads bytes at offset within the target's piece into dst; decrements local
   once they are in place (within the owner's process, and for 0 bytes,
   before CmiGet returns). The target's counter is not touched. */
CMI_REGION_CHECK_RESULT int CmiGet(void *dst, size_t bytes,
                                   const CmiLayout *dstLayout, CmiAttachment a,
                                   size_t offset, CmiCounter local,
                                   void *stream);
/* A contribution with no data: decrements the target's counter. */
CMI_REGION_CHECK_RESULT int CmiSignal(CmiAttachment a);

/* Between processes, a CmiPut of at most this many bytes travels as one
   message carrying the data and the signal, a larger one as a put with
   signal: +put_message_max_bytes, by default 0 where the put with signal is
   one network operation, and where it is emulated (a write, then a message)
   the most data whose message fits one eager send. SIZE_MAX where every put
   to another process is a message (+nordma, no put with signal). */
size_t CmiRegionPutMessageMaxBytes(void);

/* Quiescence detection (design 4.8), for Charm++: fn(created, processed) is
   called with what this process created and processed. Created: a put or
   signal when issued, into a counter with an action, or to another process
   (plus one for a local counter with an action); a get to another process
   when issued. Processed: a round's N when its action starts; a
   contribution to another process with no round (a put without a counter,
   or into a counter with no action) where it lands; a get at its
   completion; the unconsumed contributions of a counter destroyed in a
   partly filled round. Within a process, a contribution with no round is
   not counted. The two balance, summed over the processes, once nothing is
   in flight and every fired round's action has started. fn runs on any PE
   thread of the process, from the scheduler and from completion callbacks
   at poll time, so it must be thread-safe and must not block. Install it
   before the first contribution, or the counts never balance.
   Process-wide; NULL removes it. */
typedef void (*CmiRegionQdFn)(int created, int processed);
void CmiRegionSetQdHook(CmiRegionQdFn fn);

#ifdef __cplusplus
}
#endif

#endif /* CONV_REGION_H */
