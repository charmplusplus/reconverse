# Regions, counters and targets

This chapter describes the Converse layer of the persistent family: regions,
counters, targets and attachments (`include/conv-region.h`), as implemented in
`src/conv-region.cpp` (counters, regions, targets, the data paths) and
`src/conv-region-attach.cpp` (attachments and the attach protocol), with the
shared definitions in `src/conv-region-internal.h`. The design, including the
parts not built yet, is charm's `doc/persistent-family-design.md`; section
numbers below refer to it.

What exists today: contiguous host memory, with the origin on the owner's PE,
on another PE of the owner's process, or in another process. Another process
is reached through the network, even on the same host with `+ipc`: the
shared-memory paths (a counter table in a shared area, xpmem attachments) are
not built yet. Strided pieces, variable-size pieces, device memory and streams
are refused where they would be given.

## Objects

| object | where it lives | who touches it |
|---|---|---|
| counter line (`CounterLine`) | the counter table (below) | the owner PE, and every thread that contributes |
| counter record (`CmiCounterRec`, what a `CmiCounter` points to) | heap of the owner's process | the owner PE only |
| region record (`CmiRegionRec`) | heap of the owner's process | the owner PE only |
| target (`CmiTarget`) | a 160-byte value, copied into messages | read by origins |
| registration (`CmiAttachRec`) | the origin process's attachment table | its PEs, under the table lock, and its in-flight count |
| attachment (`CmiAttachmentRec`, what a `CmiAttachment` points to) | heap of the origin's process, never freed | the origin |

Only atomics are written by more than one thread: the fields of a counter
line, a region slot's landed count, an attachment's contribution count, and
a registration's state and in-flight count. Everything else has one writer,
which is why the owner's calls (`CmiRegionCreate`, `CmiRegionAddPiece`,
`CmiCounterCreate`, `CmiCounterRearm`, `CmiTargetMake`, the destroys) abort on
any other PE.

## The counter table

Each PE has a slice of up to 65536 counters, the range a 16-bit network
signal can name (5.5); `+counter_table N` lowers the limit. A counter's index
is its place in its owner's slice. A slice is a table of pointers to chunks,
each a page of lines (64 lines of 64 bytes on a 4 KiB page, 128 lines of 128
bytes on Apple's 16 KiB pages). The owner allocates a chunk the first time it
needs an index in it, and publishes it with a release store; every lookup
(`lineOf(rank, index)`) goes through the chunk table with an acquire load. No
chunk is freed before the process exits, so a late lookup never reaches freed
memory. Each PE has a second table of the same kind, one 64-bit count per
region slot (up to 65536 regions): the puts without a counter that have
landed in the region.

A line is one cache line (`CMI_CACHE_LINE_SIZE`) and holds only what a
contribution reads or writes:

| field | meaning |
|---|---|
| `word` | contributions still expected in the armed round |
| `generation` | incremented when the counter's destroy completes |
| `armedRounds` | rounds armed so far (the round check) |
| `flags` | `LIVE` while a counter owns the line; `CHECK_ROUNDS` |
| `ownerRank`, `index` | where the counter's message goes, and its number |
| `fireMsg` | the counter's message; null for a counter with no action |

Indices are reused least recently freed first (a FIFO of freed indices), and a
reused index has a new generation.

## Counting and firing

A contribution of weight k (1 for a put or a signal) is `contribute()`:

1. read `fireMsg`;
2. `old = word.fetch_sub(k, acq_rel)`;
3. if `old == k`, it completed the round: `CmiPushPE(ownerRank, fireMsg)`.

The push is the whole of firing. It goes to the owner PE's thread queue,
which any thread may push to, and the scheduler takes it from there; an
action never runs inside a `CmiPut`. `fireMsg` is read before the decrement so
that a destroy completing right after it cannot hand the contributing thread
another counter's message.

The message is a `FireMsg` (rank, index, generation). Each index has one,
made for the first counter with an action there, reused by every later one
with the new generation, and never freed, so that a push that comes late (in
an erroneous program) never reaches freed memory. Its handler, `fireHandler`,
on the owner PE:

1. finds the record; if the counter is gone or the generation differs, the
   counter was destroyed while its message waited, and the handler gives the
   index back to the free FIFO (below);
2. counts the round as started (`roundsStarted`) and reports the round's N
   to the QD hook as processed;
3. for `CMI_COUNTER_AUTO_REARM`, re-arms with the same N, which may queue the
   message again (the next round had already arrived);
4. runs the action: calls the function, or allocates a `CmiCounterFiredMsg`
   and calls the handler with it inline, which frees it.

The message is never handed to user code, so one message per counter is
enough: the counter cannot fire again until its handler has started, because
a re-arm is legal only from then on.

### Re-arm

`CmiCounterRearm(c, n)` on the owner PE: `roundsArmed++`, `armedRounds` is
stored, `word.fetch_add(n)`; a result of 0 or less means the next round (or
more) had already arrived, and the message is queued at once; each action's
re-arm then counts the next. The re-arm is refused, in every build, while
`roundsStarted != roundsArmed`: the armed round's message has not been
handled, and queuing it a second time would corrupt the queue. A counter with
no action has no message; it may be re-armed once its value is at most 0.

The record also keeps `sumArmed` and `sumStarted`, the N summed over the
rounds armed and over those whose action started. `sumArmed - word` is the
number of contributions that have reached the word, at any time, without a
second atomic; the destroy accounting below uses it.

## Regions and targets

A region record holds the base, size and kind, its pieces (offset and size,
in the order added), its slot (its index in the owner PE's region table) and
the slot's generation. In a job of several processes with one-sided support
(not `+nordma`), `CmiRegionCreate` registers the memory with the network on
the owner PE's device (`comm_backend::registerMemory`, `getRMR`); the destroy
deregisters it. `CmiTargetMake(r, piece, c)` copies into a target everything
an origin needs without asking the owner: the owner process and PE, the
region's slot and generation, the piece's address in the owner's process,
its displacement from the region's base and its size, the region's remote
key, the counter's index and generation, and the signal that names the
counter (or, for a target with no counter, the region). A target with both a
region and a counter pairs them: each records the other, for the destroy
rule below.

## The data paths within a process

`CmiPut` checks the attachment, refuses what is not implemented
(`CMI_REGION_EUNSUPPORTED`) and a put past the piece's end
(`CMI_REGION_ECAPACITY`), and then, on the calling PE:

1. counts the contribution on the attachment, for its detach notice;
2. checks the target's counter: its line must be `LIVE` with the target's
   generation, in every build (the line is about to be written anyway);
3. with `CHECK_ROUNDS` in an error-checking build, the round check;
4. reports a created contribution to the QD hook, if the counter has an
   action, and one more if `local` has one;
5. copies the source into the piece;
6. `contribute()` on the target's counter, then on `local`.

The source may be reused when `CmiPut` returns. Memory ordering: the copy
happens before the acq_rel decrement; each later decrement continues the
release sequence, the firing one synchronizes with every earlier
contribution, and the queue push and the owner's pop carry that to the owner
PE (5.4). The same PE and another PE of the process run the same code.

`CmiSignal` is the same without a copy. `CmiGet` copies the piece into the
destination and decrements `local`; it does not touch the target's counter.

## The paths to another process

Only the decrement, and the push of the counter's message, run where the
network is polled (W7): on whichever PE thread of the owner's process holds
the device.

| call | how | at the owner |
|---|---|---|
| `CmiPut` | a put with signal (`comm_backend::issueRputSignal`) into the region's registration, at the piece's displacement plus the offset | the signal's handler finds the counter by (PE, index) and calls `contribute()`; for a target with no counter, it counts the region slot's landed put |
| `CmiPut`, otherwise | one message carrying the data, sent to the owner PE | its handler, in the scheduler, copies the data into the piece and contributes, or counts the landed put |
| `CmiSignal` | an active message (`comm_backend::issueAm`) | its handler, at poll time, checks the counter's generation and contributes |
| `CmiGet` | `comm_backend::issueRget` from the owner's address | nothing; the reader's completion decrements `local` |
| `CmiGet`, without one-sided support | a request to the owner PE | its handler copies the piece into the reply; the reply's handler copies it into the destination |

The two put-with-signal kinds, "counter" (the signal is a counter index) and
"region" (the signal is a region slot), are registered with the backend
(`comm_backend::registerPutSignalKind`) at start-up, before the PE threads
start, the same kinds in the same order in every process. Converse's own
kinds (`CmiRegisterRputSignalKind`) take the backend's numbers too, so the two
do not collide. A put with signal reaches a given PE's counters because the
backend gives each (kind, local rank) its own remote completion; the 16-bit
signal then names one of that PE's 65536 counters or regions (5.5).

**Which way a put goes.** A put travels in a message where the backend has no
put with signal (`+nordma`, `+lci_no_put_signal`, a backend without it), for
zero bytes, and for any put of at most `+put_message_max_bytes`. That
threshold exists for networks where LCI emulates the put with signal (cxi
without native `fi_writedata`): there each signalled write waits for its
delivery completion before LCI sends the signal in a message, about one round
trip more, so a small put is quicker as one message that carries both. Its
default is 0 where the put with signal is one network operation
(`comm_backend::isPutSignalNative()`), and 64 bytes, LCI's inject limit,
where it is emulated, until measurements on such a network replace it.
`+region_verbose` prints, once per process and way, how a put went.

**Source completion.** Within a process, and for a put in a message, the
source may be reused when `CmiPut` returns, and `local` is decremented then.
For a put with signal, `local` is decremented by the backend's local
completion: inside `CmiPut` for a put LCI copies into a packet (up to its
buffer-copy size), at poll time otherwise. A local counter is not
count-checked: a completion that arrives before its re-arm is no readiness
error and counts toward its next round.

## Attachments and the attach protocol

The attachments of one process to one region (or, for a counter-only target,
one counter) of one generation share a registration, a `CmiAttachRec` in the
process's attachment table, keyed by owner process, owner PE, kind, slot and
generation. Each `CmiAttachTarget` returns an attachment holding a copy of
the target and a reference to the registration. Detached attachments go to a
list that later attachments take from, and are never freed, so that the
check of an attachment detached already reads memory that is still the
runtime's.

Messages, all sent urgent (`CmiSyncSendAndFreeUrgent`) so that they do not
wait behind application work:

| message | when | from, to |
|---|---|---|
| registration | the first attachment of a process to an object | origin process to owner PE |
| detach notice | the last attachment of a process is detached, or the owner asked | origin process to owner PE |
| detach request | a registration for a destroyed generation, or a destroy with `CMI_DESTROY_DETACH_ALL` | owner PE to the origin process's first PE |

Registrations and notices are batched: one message carries every entry for
one owner process, goes to the first entry's PE, and that PE forwards the
other PEs' entries to them. `CmiAttachTargets` and `CmiDetachTargets` send
one message per owner process this way.

A detach notice carries what the process contributed through the
registration (design 10.2): per counter, the contributions through every
attachment (each attachment counts its own), and, when the owner is in
another process, the puts without a counter. The notice waits for the
registration's network operations (puts with signal and gets) to complete
locally. Their count and a pending bit share one atomic: the detach sets
the bit, and the completion that brings the count to zero with the bit set
is the only one that sees that value, so it alone queues the notice on the
PE that owes it, and touches the registration no more.

## Destroy

The owner keeps, per object, a balance per origin process: registrations
minus notices. No step relies on the order in which two messages arrive: a
notice that arrives first leaves the balance at -1 until its registration
does. A destroy (`CmiRegionDestroy`, `CmiCounterDestroy`) marks the object and
completes once

- every balance is 0;
- every contribution the notices report has landed: for a counter, its
  `sumArmed - word` has reached the contributions its notices report; for a
  region, its landed count has reached the puts without a counter its
  notices report, and each counter paired with it has reached its count;
- for a counter, every region it was paired with has completed its destroy,
  since origins attached to those regions may still decrement it.

Contributions land at poll time, on threads that know nothing of the
destroy, so a destroy that waits only for them looks again on a timer
(`CcdCallFnAfter`, 50 us).

The landed counts are exact for a counter paired with the one region being
destroyed: every contribution to it came through that region, and its destroy
may be called at any time after the application stops putting. A counter
that also serves another region still in use, or a target with no region,
counts their contributions too, which can make up for a put of this region
still in flight. For that case the rule is the application's (design 10.2):
destroy the region only after the counter has fired the last round that a
put into the region counted toward. That firing means every contribution of
the round, this region's among them, has landed.

For example, a stencil block receiving 6 ghost faces from 6 neighbors, with
one counter of N = 6:

- if the faces are pieces of one allocation, that is one region, which the
  rule does not concern;
- if they are 6 separately allocated buffers, that is 6 regions sharing the
  counter. When one neighbor changes, the owner destroys that face's region
  in the code that runs after the step's 6 faces have arrived (the counter's
  action, or later), not right after sending its own faces: until the
  counter fires, the other 5 faces' contributions can carry the landed count
  past what the old neighbor reported, the destroy completes, and the old
  neighbor's last face can land in memory the application has freed.

Completion frees the slot or index (with a new generation), deregisters the
region, frees runtime-allocated memory, and queues a message that runs
`done(arg)` on the owner PE. A counter destroyed while its message is
queued, or about to be (its round fired and its action has not started),
withholds its index until the message's handler has run, so that no later
counter there queues the message twice. A registration that arrives for a
generation already destroyed gets a detach request; a notice for one is
dropped. An origin the owner had not heard of when the destroy completed
has, by the application's own logic, nothing in flight (10.2, step 1).

A counter destroyed in a partly filled round, or whose last round fired and
has not run, reports its unconsumed contributions to the QD hook as
processed: `sumArmed - word - sumStarted`.

In the origin process, a detach request marks the registration
`DetachedByOwner`, removes it from the table and sends the notice once its
operations in flight have completed. Attachments that still refer to it
stay valid memory; in an error-checking build a put through them aborts.

## Error checks

`CMI_REGION_CHECKS` comes from the CMake option `RECONVERSE_ERROR_CHECKING`:
`AUTO` (the default) turns it on where `CMK_OPTIMIZE` is 0 (a standalone Debug
build, a Charm++ build without `--with-production`), decided at every
configure; `ON` or `OFF` overrides it. It adds:

- the count check: a contribution to a target's counter that finds
  `old < k` arrived after its round fired and before the re-arm. At the
  owner it runs where the contribution lands, so a remote origin's early put
  aborts the owner's process;
- the round check, for counters created with `CMI_COUNTER_CHECK_ROUNDS`, within
  a process: the k-th contribution through one attachment aborts unless the
  owner has armed at least k rounds (`armedRounds`), which catches a later
  round's contribution counted in place of a missing one. It assumes each
  attachment contributes once per round. Across processes there is none: the
  signal names no origin and no round;
- a put, get or signal through a registration its owner ended.

Every build checks the attachment and target (`magic`), the capacity, the
counter's generation on an in-process contribution and at the owner of a
remote signal or message, a signal that names a counter or region that does
not exist, re-arm legality, and that the owner's calls run on the owner PE.
The readiness aborts name the counter, its generation, the owner PE and the
round, and end with design 11's advice to use a message send.

## Quiescence detection

Reconverse has no QD of its own; Charm++ installs a function with
`CmiRegionSetQdHook`. It is called on the PE thread where the event happens:
created(1) when a put or signal into a counter with an action is issued (and
once more for a `local` counter with an action), processed(N) when a round's
action starts, and the unconsumed contributions at a destroy. A put without a
counter to another process counts created when issued and processed where it
lands (the polling PE, or the owner PE for a message); a get to another
process, created when issued and processed at its completion. Within a
process, a contribution into a counter with no action is not counted, nor is
a put without a counter or a get, whose data is in place when the call
returns.
