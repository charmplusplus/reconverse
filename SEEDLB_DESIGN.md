# Neighborhood-Averaging Seed Balancer — Design Notes (Phase 1)

Status 2026-07-13: implemented and tested on reconverse branch
`neighborhood-averaging` (commit 0b6401d), charm branch
`reconverse-specific-build` (mac fix 9eb695695). Written by Claude with
L.V. Kale; companion background is `DiffusionGraphfiles/CLAUDE.md` and the
paper notes in `DiffusionGraphfiles/papers/notes/` (esp.
`synthesis_seed_lb.md`).

## 1. Problem and shape of the solution

Balance placement of NEW singleton chares ("seeds", created with CK_PE_ANY)
across PEs, inside the runtime. Phase 1 is unprioritized; Phase 2 adds
bitvector priorities and prioritized exchange (IPPS'93 two-quanta protocol).

Three cooperating pieces:
1. a **virtual topology** — C-regular random graph, one vertex per PE;
2. a **per-PE seed deque** — where CK_PE_ANY seeds wait;
3. a **sender-initiated averaging protocol** — moves seeds along graph
   edges toward the neighborhood average (Saletore DMCC'90, Fig. 3).

Deliberate Phase-1 simplifications (user decisions, revisit later):
PE-based (not node-based) graph and queues — maximizes graph size N for
scaling studies and keeps concurrency trivial; hops-per-seed metric
postponed; no priorities yet.

## 2. The topology

`cldb_regular_graph.h` (trimmed from DiffusionGraphfiles/regular_graph.hh):
configuration-model generation + local switch repair, O(N·C). Every PE runs
`RegularGraph(N, C, GRAPH_SEED)` with the same constants at CldModuleInit —
deterministic, so all PEs hold the identical graph and each keeps only its
own adjacency row. No startup broadcast, no coordination. ~ms even at 32K
vertices. If N−1 ≤ C the graph degenerates to a clique of the other PEs.

Why this graph: random C-regular graphs are near-Ramanujan (λ2 ≈ 2√(C−1)),
and the spectral gap C−λ2 governs diffusion convergence; diameter is near
the Moore bound as a side effect. Spectral quality and diameter behavior
were validated offline (see DiffusionGraphfiles: MINLOC-selected generation,
reject-and-retry diameter check — those live in the Charm-level GraphTopology
library, not in this Converse-level port, which generates a single candidate).
Connectivity C is the headline experimental axis (`+cldC`, even, default 6).

## 3. The seed queue: what kind and why

**A plain `std::deque<char*>`, thread_local, one per PE, owner-only.**

- NOT the CMK_TASKQUEUE work-stealing deque: that one is intra-node only
  (steals via CpvAccessOther on same-node ranks), has no bulk-peel
  operation, and its lock-free structure buys nothing here (see next
  point). It also can't grow priority support cleanly (Phase 2).
- NOT a concurrent structure at all: only the owner PE ever touches the
  deque. Seeds arriving from neighbors come in as ordinary converse
  messages whose handler executes on the owner's scheduler thread. This is
  the "separate queue, message-mediated" alternative to the old dual-
  residency token ring — no locks, no atomics, no cross-thread races.
- Two-ended discipline (the point of a deque):
  - **execute from the TOP** (back; newest) → LIFO → depth-first traversal
    of a divide-and-conquer tree → memory ∝ tree depth, not breadth (same
    memory argument as delayed-release in the IJPP'90 paper, achieved
    structurally);
  - **export from the BOTTOM** (front; oldest) → shallowest → chunkiest
    subtasks → maximum work moved per byte and per crossing.
- Phase-2 door: the API concept is pop-for-execution / peel-for-export,
  not front/back — prioritized policies re-map which element each
  operation selects (D&C: run deep, ship shallow; speculative search: run
  highest priority, ship top-k high).

Ownership rules: `CldEnqueue(CLD_ANYWHERE)` stores the message pointer
(after `CmiSetInfo`) — no copy. Execution pops and `CmiHandleMessage`s the
original pointer; charm frees it after the constructor runs. Export
consumes and frees originals (see §6). Every seed has exactly one owner at
all times.

## 4. Scheduler integration and polling frequencies

Base: the `register-queues` table-driven scheduler. Each PE has a 64-slot
table; **each loop iteration polls exactly one slot's handler**
(`poll_handlers[loop_counter & 63]()`). Handlers are `bool fn()` returning
work-done. Relative frequencies are normalized into slot shares by
`add_list_of_handlers`.

The strategy contributes handlers through one hook, `CldAddPollHandlers()`,
called from `CmiQueueRegisterInitThread()` before the table is built (the
only scheduler change: +1 line, plus the declaration). Registered:

| handler | rel. freq | resulting share (of 64 slots, total rel 48) |
|---|---|---|
| pollConverseNodeQueue | 1 | ~1–2 |
| pollConverseThreadQueue | 16 | ~21 |
| pollNodePrioQueue | 1 | ~1–2 |
| pollThreadPrioQueue | 16 | ~21 |
| pollProgress | 4 | ~5 |
| pollTaskQueue (if built) | 1 | ~1–2 |
| **pollSeedDeque** | **8** | **~10–11** |
| **pollSeedBalance** | **1** | **~1–2** |

**How 8 was chosen (honestly: a reasoned judgment call, not a measured
optimum — it deserves a knob and a sweep).** The constraints:
- Message queues (rel 16) should outrank the seed deque: a response/data
  message usually *completes* existing work while a seed *starts new* work;
  draining messages first keeps the depth-first discipline and bounds both
  memory and the tree of in-flight subtrees.
- The share must be non-trivial: when all queues are empty, the loop sweeps
  64 empty slots in ~sub-µs, so any nonzero share lets an idle PE start a
  seed almost immediately. But under LOAD, one iteration = one executed
  grain — a rare seed slot would starve seed starts behind long message
  streams. 8 (≈1/6 of slots) means at most ~5 message-handling iterations
  separate consecutive seed opportunities.
- pollSeedBalance at rel 1 is a *fallback* only: the primary balance
  trigger is a `maybeBalance()` call after EVERY executed seed (inside
  pollSeedDeque), so a loaded PE reacts within one grain. The table slot
  matters only when the deque is empty (PE idle → sweeps are fast → the
  slot fires every few µs anyway). It returns false always (bookkeeping ≠
  work) so idle detection stays truthful.

Known scheduler-semantics caveat: one-handler-per-iteration means a slot
whose queue is empty does nothing that iteration even if another queue has
work; the fast empty-sweep makes this cheap, but frequency tuning interacts
with grain size — worth a dedicated experiment.

## 5. The balancing protocol

DMCC'90 neighborhood averaging, sender-initiated:
- `maybeBalance()`: avg = (myLoad + Σ nbrLoad)/(|nbrs|+1) (integer floor —
  KNOWN WART: stalls transfers below ~|nbrs| load; fix with ceil/double).
  If myLoad > avg: for each neighbor with nbrLoad < avg, export
  min(avg − nbrLoad, surplus, batchMax) seeds bottom-first as ONE batch;
  update nbrLoad optimistically (+= sent) to prevent double-sending before
  the neighbor's own status arrives.
- Load metric: deque length (the running grain is deliberately excluded;
  a PE that just went empty is "free soon").
- **Load information is pushed, never pulled**: (a) piggybacked on every
  batch header (srcLoad after export); (b) explicit StatusMsg to a
  neighbor only when |myLoad − lastSent[i]| ≥ statusDelta (default 2).
  No wall-clock period anywhere. NOT piggybacked on application messages
  (the balancer never touches the app's critical path).
- Gaps vs DMCC to close before big runs: no minimum-interval gate on
  status sends (delta only) — add `+cldStatusInterval`; while a PE is
  overloaded it refreshes only the under-average neighbors (stale-low
  bias in the others' views — benign direction, but measurable).
- No FirstFew startup spreading: diffusion bootstraps itself (PE0's
  surplus over an all-zero neighborhood exports within the first grains;
  spreads in ~diameter rounds ≈ log_{C−1} N).

## 6. Batching and message-memory discipline

Export packs k seeds into ONE combined message per neighbor per round
(cap `+cldBatchMax`, default 64).

**Invariant (cost: an evening of debugging): never serialize an UNPACKED
charm envelope.** Always run the CldInfoFn's pack fn (`pfn`) before
memcpy'ing an envelope into a batch — unpacked envelopes embed live
pointers and are not self-contained byte strings; the failure mode is
misrouted responses and null-object crashes far from the cause. (rand
never hits this: it either hands off the original pointer, or lets
CmiSyncSendAndFree serialize a message it packed first.)

Batch record format: `[CmiChunkHeader][seed bytes]`, ALIGN_BYTES-aligned,
where the embedded header's ref field holds a NEGATIVE offset back to the
batch allocation's header. `CmiFree`/`CmiAllocFindEnclosing` interpret
negative refs as "sub-message of enclosing block" (mechanism inherited
from old converse — it's the code under the `TODO: still needed?` comment
in convcore.cpp; the answer is yes). Offsets are relative → survive the
wire byte-copy.

Delivery policy, decided per batch at the receiver:
- **same-node batch → zero-copy**: push interior pointers into the deque;
  bump batch refcount to seed count; batch dies when its last seed is
  executed (charm frees it) or re-exported. Measured win (1×8:
  0.674→0.615 s).
- **cross-node batch → copy-and-release**: one CmiAlloc+memcpy per seed,
  free the batch immediately. Zero-copy there pins the transport's receive
  buffer until the last seed is consumed → pool starvation (measured ~15%
  slowdown on laptop 2-process runs).
- Overrides for A/B at scale: `+cldCopyAtDest` (copy everywhere),
  `+cldZCRemote` (zero-copy everywhere). Real NICs/pinned pools may move
  this tradeoff — measure.

Explicit-destination messages (fixed-PE sends, broadcasts, node-queue
variants) keep the classic rand-style paths (CldSwitchHandler +
CmiSyncSendAndFree). `CldNodeEnqueue(CLD_ANYWHERE)` is treated as
PE-anywhere (local deque).

## 7. Knobs, benchmark, build

Knobs: `+cldC` (even; default 6) · `+cldStatusDelta` (2) · `+cldBatchMax`
(64) · `+cldNoBatch` (rand-style singles; diagnostic) · `+cldCopyAtDest` /
`+cldZCRemote` · `+cldSeedStats` (BROKEN: reconverse never calls
CldCallback — needs an exit hook).

Canonical benchmark: **fib(45) threshold=30** (`apps/fib`, NOT yet under
version control): 3194 seeds, ~2.6 ms leaf grain, 3.49 s sequential.
Current: 1proc×8PE 0.615 s; 2proc×4PE 0.70–0.78 s. Per-seed overhead
~4.2 µs.

**Messaging-overhead mystery RESOLVED (2026-07-13).** Identical
converse pingpong.C on both runtimes, same machine, 512B one-way:
- same-process: reconverse 0.30 µs vs old converse 1.44 µs — reconverse
  is ~4.8x FASTER in-process (old-charm build was non-production; its
  true number improves but stays above reconverse's).
- cross-process on THIS MAC: 45 µs one-way, flat across sizes = libfabric
  TCP-loopback provider. macOS libfabric has NO shm provider (fi_info:
  tcp/sockets/udp only; log: "Using tcp provider"). NOT a reconverse
  defect. The 64KB ≈ 3 ms pathology is the same TCP path.
- CONSEQUENCE FOR EXPERIMENT DESIGN: the "grain must be ~2.6 ms" rule was
  calibrated against this 45 µs mac artifact. On Linux clusters
  (fi_shm intra-node, real fabric inter-node, ~1-2 µs), viable grain is
  ~20-50 µs — fib t≈20 should work. Use 1-process multi-PE for laptop
  measurements; 2-process laptop runs are functional tests only.
  (Earlier ping_ack "~80 µs same-process" was that test's accounting
  artifact — disregard.) Mac-local fix if ever needed: reconverse's
  dormant CMK_USE_SHMEM IPC path (cmishmem.cpp).

Build (mac): `./build charm++ reconverse-darwin-arm8
--with-fetch-reconverse-dir=$PWD/reconverse --with-production -j8`
(strategy selected by CMake cache var `RECONVERSE_CLDB`, default
neighborhood). Run: `lcrun -n <procs> env DYLD_LIBRARY_PATH=<charm>/lib
./fib +pe <PEs> <n> <t> [+cld...]`.

## 8. Open items (priority order for the cluster phase)

1. `+cldStatusInterval` (DMCC's second suppression gate) + balance-eval
   period knob + polling-frequency knob for pollSeedDeque.
2. Fix `+cldSeedStats` (exit hook) — needed to observe status volume and
   import/export counts at scale.
3. avg integer-floor fix (matters at small loads / large C).
4. hops-per-seed counter (postponed) — the DMCC stability metric.
5. Investigate the ~80 µs same-process message-path latency and the 64KB
   pingpong pathology (reconverse-level, benefits everything).
6. Handler-registration-order assert (indices must match across PEs).
7. Put apps/fib under version control.
8. Phase 2: priorities (bitvector; two-quanta exchange: unconditional
   top-k + load-conditional bulk; flush-below-priority; adherence metric =
   nodes-created variance vs P).
