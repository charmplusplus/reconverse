# Shared-memory IPC: the `+ipc` pool

This chapter describes how Reconverse moves a message between two processes
on the same host when `+ipc` is on. It covers the code in `src/cmishmem.cpp`
and `src/cmi-shmem-common.h`. How each process's segment is created and
mapped is in `src/cmishm.cpp` (POSIX shared memory) and `src/cmixpmem.cpp`
(XPMEM). The options are in the README, under "Shared-memory IPC".

## Segments and blocks

Every process owns one segment of shared memory: a header (`ipc_shared_`)
followed by `++ipcpoolsize` bytes for blocks (see "Memory use" for the
default). Every other process on the host maps that segment into its own
address space. A message sent to process P is copied into a block in **P's**
segment, and P's PEs run it from there without another copy.

Each process maps a segment at a different address, so everything in a
segment that refers to another place in it, including the links between
blocks, is an offset from the segment's start.

A block is a `CmiIpcBlock` header followed by space for one message. Block
sizes are powers of two (`kCutOffPoints`); the pool uses the first
`kIpcRetBins` of them, 64 bytes to 2 MiB. A block is cut from the unused part
of the segment (the heap) the first time a sender needs one of that size, and
keeps its size for the rest of the run. It is never merged, split or returned
to the heap.

Fields of `CmiIpcBlock` used by the pool:

| field | meaning |
|---|---|
| `segment_proc` | the process whose segment holds the block (the receiver) |
| `sender_proc` | the process that allocated it (the sender) |
| `owner` | host-local index of the PE that allocated it; the block returns to that PE |
| `orig` | the block's own offset in the segment |
| `next` | offset of the next block on whatever list holds the block |
| `size` | the block's size class |

## The lists in a segment

A segment's header holds:

- `heap`: offset of the start of the unused part of the segment. `max` is the
  end.
- `rq[rank]`, one per PE of the owning process: blocks holding messages for
  that PE.
- `node_rq`: blocks holding messages for the process as a whole
  (`CmiSyncNodeSend*`).
- `ret[i].bin[b]`, one list per PE of the host (`i` is its host-local index)
  and block size `b`: empty blocks of size `b` that PE `i` allocated in this
  segment and whose messages have since been freed.

All of these lists are singly linked stacks of blocks, ended by
`cmi::ipc::max`. PEs of several processes write them, so each receive-queue
head has a cache line to itself (`ipc_list_`), and each sender's 16 free-list
heads share two cache lines (`ipc_return_`).

Only two operations touch a shared list:

1. **Push** one block (`pushList_`): set `block->next` to the head just read,
   then compare-and-swap the head from that value to the block. Any number of
   PEs may push at once; a PE whose swap fails rereads the head and retries.
2. **Take** the whole list (`takeList_`): swap the head with "empty" in one
   exchange and walk what came back.

No shared list ever has a single block removed from it. That is what makes
the lists safe without a lock. Removing one block means reading the head and
its `next`, then swapping; between the read and the swap, another PE could
take the block, reuse it and put it back, leaving a stale `next`. Taking the
whole list has no second value to go stale, and a push needs only the current
head. The push releases and the take acquires, so a taker sees the message
(or, for a free list, the block header) that the pusher wrote.

Each list has one taker, or, for `node_rq`, several takers that each get a
different part:

| list | holds | pushed by | taken by |
|---|---|---|---|
| `rq[r]` | blocks with messages for rank `r` | any sender on the host | rank `r` |
| `node_rq` | blocks with node-level messages | any sender on the host | any PE of the process |
| `ret[i].bin[b]` | empty blocks of size `b` allocated by host PE `i` | the PEs of the owning process, when they free a message | host PE `i` |

Each PE also keeps state that no other thread touches (`ipc_pe_state_`):

- `cache[h][b]`: a private list of empty blocks of size `b` in the segment of
  the host's `h`-th process (`CmiIpcManager::hostProc` maps a process to `h`),
  ready for reuse.
- `inbox`: blocks taken from its receive queues and not yet delivered.
- `hostIdx`: its index among the host's PEs, which names its `ret` entry in
  every segment.

Limits: when a host runs more than one process, each may have at most
`kMaxIpcRanks` (512) PEs and the host at most `kMaxIpcHostPes` (1,024);
`finishSetup_` stops the run at start-up past either. A process alone on its
host has no peer to use the pool with, so the limits do not apply to it and
its PEs never poll the pool.

## Sending

`CmiIpcTrySendAndFree` sends a message from PE A to rank `r` of process P
through the pool when all of these hold:
- P is on the same host;
- the pool is ready;
- the message is at most the cutoff (`++ipccutoff`, at most 1 MiB);
- A is not inside a `CmiIpcBeginNetworkOnly` scope.

Otherwise the caller sends it over the network.

Allocation (`CmiAllocIpcBlock`) of a block of size class `b` in P's segment:

1. Pop from A's private list for P and `b`.
2. If that is empty, take the whole of `ret[A].bin[b]` in P's segment as the
   new private list, and pop from it. Before touching the segment at all, A
   checks that P has published its heap offset (see "Start-up").
3. If that is empty too, cut a new block from P's heap: read `heap`, check
   that the block fits below `max`, and compare-and-swap `heap` forward. If
   it does not fit, allocation fails, the message goes over the network and
   `CmiIpcPoolFullSends` counts it (see "Memory use").

A then copies the message into the block, writes the destination rank into
the message header, and pushes the block onto `rq[r]` (or `node_rq`) in P's
segment (`CmiPushIpcBlock`).

## Receiving

Once per scheduler pass, `CmiDeliverIpcInbox` on rank `r` of P takes its whole
`rq[r]` and the whole `node_rq` into its inbox, then delivers every block it
took. A taken list comes back newest first and the inbox is popped from the
back, so each list's blocks are delivered oldest first.

Delivery (`CmiDeliverIpcBlockMsg`) puts the global PE number back in the
header, so a handler cannot tell a pool message from a network one. It then
pushes the message onto the PE's (or the process's) local queue.

When the message is freed, `CmiFreeIpcBlock` pushes the block onto
`ret[owner].bin[size]` in the same segment. Its allocating PE finds it there
at step 2 of a later allocation.

## Order

Messages from one PE to one PE arrive in the order sent. The sender pushes
them in order onto the receiver's own queue, and each take delivers its
blocks in push order; a later take holds only later pushes. Messages from
different senders, and node messages relative to PE messages, have no defined
order. `tests/ipc_order` checks the per-pair order with one and with several
PEs per process.

## Memory use

A freed block goes back only to the PE that allocated it, and blocks never
return to the heap. Once PE A has cut blocks from P's segment, those blocks
carry only A's messages to P. So P's segment is in effect divided among its
senders, first come first served.

Once the heap is used up, a sender can still send to P through the pool as
long as its own freed blocks of the right size are there. A sender that has
none, for example one that first needs a block after another sender's burst
used up the heap, sends to P over the network for as long as that lasts.
Nothing hangs and nothing is lost, but those sends do not get the pool's
speed.

Three things make this visible and rarer:

- `CmiIpcPoolFullSends()` counts this PE's sends that went over the network
  because the destination's segment was full.
- `CmiIpcPoolBytesUsed()` gives the bytes of this process's segment that
  senders have cut into blocks so far.
- The first such send in a process prints a notice naming the segment and
  `++ipcpoolsize`.

Without `++ipcpoolsize`, a segment's size grows with the number of PEs that
can send to it: 512 KiB (`kSegmentPerSender`) for each PE of the host's other
processes, and at least 8 MiB.

| PEs per host (processes × PEs) | senders per segment | default segment |
|---|---|---|
| 2 × 2 | 2 | 8 MiB |
| 2 × 23 | 23 | 11.5 MiB |
| 4 × 16 | 48 | 24 MiB |
| 8 × 16 | 112 | 56 MiB |

Both mechanisms back a segment with pages that are allocated when first
written, so space no sender reaches costs address space, not memory. Every
process on a host computes the same size, which matters because any of them
may create and size a peer's segment (`sizeSegmentForHost_`, called from
`CmiMakeIpcManager` once the topology is known).

The header costs 64 bytes per receive queue and 128 bytes per host PE, about
160 KiB at the limits above.

## Start-up

A peer can map P's segment before P has initialized it, and the segment then
reads as zeros, which the lists would take for real offsets. The constructor
of `ipc_shared_` initializes every list and stores `heap` last, with release
ordering. A sender that reads `heap` as `cmi::ipc::nil` treats the segment as
not ready (`CMI_IPC_TIMEOUT`); `CmiMsgToIpcBlock` retries a few times
(`cmi::ipc::defaultTimeout`), then the message goes over the network.
`CmiIpcManager::ready` is set once
every peer segment on the host is mapped; until then sends use the network.
