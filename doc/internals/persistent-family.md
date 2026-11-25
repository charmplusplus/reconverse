# Regions, counters and targets

This chapter describes the Converse layer of the persistent family: regions,
counters, targets and attachments (`include/conv-region.h`), as implemented in
`src/conv-region.cpp` (counters, regions, targets, the data paths) and
`src/conv-region-attach.cpp` (attachments and the attach protocol), with the
shared definitions in `src/conv-region-internal.h`. The design, including the
parts not built yet, is charm's `doc/persistent-family-design.md`; section
numbers below refer to it.

What exists today: contiguous host memory, with the origin on the owner's PE
or on another PE of the owner's process. Targets of another process are
refused at attach. Strided pieces, variable-size pieces, device memory and
streams are refused where they would be given.

## Objects

| object | where it lives | who touches it |
|---|---|---|
| counter line (`CounterLine`) | the counter table (below) | the owner PE, and every thread that contributes |
| counter record (`CmiCounterRec`, what a `CmiCounter` points to) | heap of the owner's process | the owner PE only |
| region record (`CmiRegionRec`) | heap of the owner's process | the owner PE only |
| target (`CmiTarget`) | a 160-byte value, copied into messages | read by origins |
| registration (`CmiAttachRec`) | the origin process's attachment table | its PEs, under the table lock |
| attachment (`CmiAttachmentRec`, what a `CmiAttachment` points to) | heap of the origin's process | the origin |

Only atomics are written by more than one thread: the fields of a counter
line, and a registration's state. Everything else has one writer, which is
why the owner's calls (`CmiRegionCreate`, `CmiRegionAddPiece`,
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
memory.

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

The message is a `FireMsg` (rank, index, generation), allocated when the
counter is created and owned by the runtime. Its handler, `fireHandler`, on
the owner PE:

1. finds the record; if the counter is gone or the generation differs, a
   destroy left the message to it, and it frees it;
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
stored, `word.fetch_add(n)`; a result of 0 means the next round had already
arrived in full, and the message is queued at once. The re-arm is refused,
in every build, while `roundsStarted != roundsArmed`: the armed round's
message has not been handled, and queuing it a second time would corrupt the
queue. A counter with no action has no message; it may be re-armed once its
value is at most 0.

The record also keeps `sumArmed` and `sumStarted`, the N summed over the
rounds armed and over those whose action started. `sumArmed - word` is the
number of contributions that have reached the word, at any time, without a
second atomic.

## Regions and targets

A region record holds the base, size and kind, its pieces (offset and size,
in the order added), its slot (its index in the owner PE's region table) and
the slot's generation. `CmiTargetMake(r, piece, c)` copies into a target
everything an origin needs without asking the owner: the owner process and
PE, the region's slot and generation, the piece's address in the owner's
process and its size, and the counter's index and generation. A target with
both a region and a counter pairs them: each records the other, for the
destroy rule below.

## The data paths within a process

`CmiPut` checks the attachment, refuses what is not implemented
(`CMI_REGION_EUNSUPPORTED`) and a put past the piece's end
(`CMI_REGION_ECAPACITY`), and then, on the calling PE:

1. checks the target's counter: its line must be `LIVE` with the target's
   generation, in every build (the line is about to be written anyway);
2. with `CHECK_ROUNDS` in an error-checking build, the round check;
3. reports a created contribution to the QD hook, if the counter has an
   action, and one more if `local` has one;
4. copies the source into the piece;
5. `contribute()` on the target's counter, then on `local`.

The source may be reused when `CmiPut` returns. Memory ordering: the copy
happens before the acq_rel decrement; each later decrement continues the
release sequence, the firing one synchronizes with every earlier
contribution, and the queue push and the owner's pop carry that to the owner
PE (5.4). The same PE and another PE of the process run the same code.

`CmiSignal` is the same without a copy. `CmiGet` copies the piece into the
destination and decrements `local`; it does not touch the target's counter.

## Attachments and the attach protocol

The attachments of one process to one region (or, for a counter-only target,
one counter) of one generation share a registration, a `CmiAttachRec` in the
process's attachment table, keyed by owner process, owner PE, kind, slot and
generation. Each `CmiAttachTarget` returns a new attachment holding a copy of
the target and a reference to the registration.

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

The owner keeps, per object, a balance per origin process: registrations
minus notices. No step relies on the order in which two messages arrive: a
notice that arrives first leaves the balance at -1 until its registration
does. A destroy (`CmiRegionDestroy`, `CmiCounterDestroy`) marks the object
and completes once every balance is 0; a counter also waits until every
region it was paired with has completed its destroy, since origins attached
to those regions may still decrement it. Completion frees the slot or index
(with a new generation), frees runtime-allocated memory and the counter's
message, and queues a message that runs `done(arg)` on the owner PE. A
registration that arrives for a generation already destroyed gets a detach
request; a notice for one is dropped. An origin the owner had not heard of
when the destroy completed has, by the application's own logic, nothing in
flight (10.2, step 1).

A counter destroyed in a partly filled round, or whose last round fired and
has not run, reports its unconsumed contributions to the QD hook as
processed: `sumArmed - word - sumStarted`. If its message is queued, the
handler frees it when it finds the counter gone.

In the origin process, a detach request marks the registration
`DetachedByOwner`, removes it from the table and sends the notice at once.
Attachments that still refer to it keep valid memory; in an error-checking
build a put through them aborts.

## Error checks

`CMI_REGION_CHECKS` is set from the CMake option `RECONVERSE_ERROR_CHECKING`,
on by default where `CMK_OPTIMIZE` is 0 (a standalone Debug build, or a
Charm++ build without `--with-production`). It adds:

- the count check: a contribution that finds `old < k` arrived after its
  round fired and before the re-arm;
- the round check, for counters created with `CMI_COUNTER_CHECK_ROUNDS`: the
  k-th contribution through one attachment aborts unless the owner has armed
  at least k rounds (`armedRounds`), which catches a later round's
  contribution counted in place of a missing one. It assumes each attachment
  contributes once per round;
- a put, get or signal through a registration its owner ended.

Every build checks the attachment and target (`magic`), the capacity, the
counter's generation on an in-process contribution, re-arm legality, and
that the owner's calls run on the owner PE. The readiness aborts name the
counter, its generation, the owner PE and the round, and end with design
11's advice to use a message send.

## Quiescence detection

Reconverse has no QD of its own; Charm++ installs a function with
`CmiRegionSetQdHook`. It is called on the PE thread where the event happens:
created(1) when a put or signal into a counter with an action is issued (and
once more for a `local` counter with an action), processed(N) when a round's
action starts, and the unconsumed contributions at a destroy. Contributions
within a process into a counter with no action are not counted, nor are puts
without a counter, whose data is in place when `CmiPut` returns.
