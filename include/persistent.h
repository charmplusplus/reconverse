/*****************************************************************************
                  Persistent Communication API

  Persistent communication is used when the same point-to-point message
  channel is used repeatedly, so that the per-message setup (allocating a
  receive buffer, registering memory, matching the message) can be paid once
  instead of on every send.

  A channel is described by a PersistentHandle. Setting one up allocates
  PERSIST_BUFFERS_NUM buffers of maxBytes each on the destination PE, and
  (when the communication backend supports RMA) registers them so the sender
  can write straight into them with a one-sided put. Backends without RMA
  support emulate the same behavior with ordinary Converse messages.

  * PersistentHandle CmiCreatePersistent(int destPE, int maxBytes):
        Sender initiates the setup. Returns immediately; messages sent on the
        handle before the destination has answered are buffered locally and
        flushed once the channel is ready.
        A destination on the calling PE's own node needs none of this: those
        PEs share an address space, so the ordinary send path already hands
        the message over by pointer, which no buffered scheme can beat. Such a
        channel is set up for free, allocates nothing on the destination, and
        forwards to that path. The handle is still valid and still usable with
        every call below, so callers need no special case for it.
  * PersistentReq CmiCreateReceiverPersistent(int maxBytes);
    PersistentHandle CmiRegisterReceivePersistent(PersistentReq req);
        Receiver initiates the setup. The receiver calls
        CmiCreateReceiverPersistent(), sends the returned PersistentReq to the
        sender (it is plain data, so it can be copied into a message), and the
        sender turns it into a usable PersistentHandle with
        CmiRegisterReceivePersistent().
  * void CmiUsePersistentHandle(PersistentHandle *p, int n):
        Route the following sends through the array of handles "p" (of size
        n). n == 1 sends every message on the same channel; n > 1 walks the
        array, one handle per PE, which is what a multicast needs. Passing
        p = NULL cancels persistent sending.
  * void CmiDestroyPersistent(PersistentHandle h):
        Tear down one channel, freeing the buffers on the destination PE.
  * void CmiDestroyAllPersistent():
        Tear down every channel this PE takes part in.

  Ownership of a received message
  -------------------------------
  A message delivered over a persistent channel lives in one of the channel's
  receive buffers, so it is not an ordinary CmiAlloc'd block. It is still used
  exactly like one: the handler must either CmiFree() it or hand it to a
  send-and-free call when it is done. That release is what returns the buffer
  to the sender for reuse, so a handler that keeps the message forever stalls
  the channel. The buffer itself is never freed by CmiFree; it is released
  when the channel is destroyed. A message that arrived over a same-node
  channel is an ordinary allocation and CmiFree simply frees it, which is why
  a handler needs no idea which kind it was given.
 *****************************************************************************/

#ifndef RECONVERSE_PERSISTENT_H
#define RECONVERSE_PERSISTENT_H

#include "converse.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMK_PERSISTENT_COMM 1

/* Number of receive buffers allocated per channel. This is how many messages
   the sender can have in flight before it has to wait for the receiver to
   release a buffer. */
#define PERSIST_BUFFERS_NUM 4

/* Bytes reserved for a serialized remote memory region handle. Must be large
   enough for whatever comm_backend::getRMR() produces. */
#define CMK_PERSISTENT_RMR_BYTES 64

/* Reference count base used to mark a persistent receive buffer. CmiFree()
   recognizes counts above this as "buffer, not allocation" and releases the
   buffer back to its sender instead of freeing memory. */
#define CMK_PERSISTENT_REFBASE 0x40000000

typedef void *PersistentHandle;

/* Everything the sender needs to reach one receive buffer. Plain data, so it
   can be memcpy'd into a message. */
typedef struct PersistentBufDesc {
  CmiUInt8 addr; /* address of the message area in the receiver's address
                    space; informational, a remote sender addresses the buffer
                    through rmr and disp */
  CmiUInt8 disp; /* offset of the message area from the start of the
                    registered memory region, for one-sided puts */
  char rmr[CMK_PERSISTENT_RMR_BYTES]; /* serialized remote memory region */
} PersistentBufDesc;

/* Receiver-side description of a channel, handed to the sender so it can call
   CmiRegisterReceivePersistent(). Plain data, safe to copy into a message. */
typedef struct PersistentReq {
  int pe;                  /* PE that owns the receive buffers */
  int maxBytes;            /* size of each receive buffer */
  PersistentHandle myHand; /* receiver-side channel */
  PersistentBufDesc bufs[PERSIST_BUFFERS_NUM];
} PersistentReq;

/* Initialize the persistent communication module on this PE. Called during
   Converse startup; calling it again is harmless. */
void CmiPersistentInit(void);

PersistentHandle CmiCreatePersistent(int destPE, int maxBytes);
PersistentHandle CmiCreateNodePersistent(int destNode, int maxBytes);
PersistentReq CmiCreateReceiverPersistent(int maxBytes);
PersistentHandle CmiRegisterReceivePersistent(PersistentReq req);

void CmiUsePersistentHandle(PersistentHandle *p, int n);
/* Advance to the next handle of the array installed by
   CmiUsePersistentHandle() without sending anything on the current one. */
void CmiPersistentOneSend(void);

void CmiDestroyPersistent(PersistentHandle h);
void CmiDestroyAllPersistent(void);

/* Send a message on a specific channel, bypassing the handle array installed
   by CmiUsePersistentHandle(). Takes ownership of msg, as CmiSyncSendAndFree
   does. */
void CmiSendPersistentMsg(PersistentHandle h, int messageSize, void *msg);

/* --- Used by the rest of Converse, not by application code --- */

/* Called by CmiSyncSendAndFree() when a handle array is installed. Returns 1
   if the message was taken over by a persistent channel, 0 if the caller
   should send it the ordinary way. */
int CmiPersistentHandleSend(int destPE, int messageSize, void *msg);

/* Called by CmiFree() when the last reference to a persistent receive buffer
   is dropped, to hand the buffer back to its sender. */
void CmiPersistentReleaseBuffer(void *msg);

#ifdef __cplusplus
}
#endif

/* The handle array installed by CmiUsePersistentHandle(). Declared outside the
   extern "C" block so the definition in persist-comm.cpp matches. */
CpvExtern(PersistentHandle *, phs);
CpvExtern(int, phsSize);
CpvExtern(int, curphs);

#endif /* RECONVERSE_PERSISTENT_H */
