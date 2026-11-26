# reconverse
Complete an mostly backward compatible implementation of a new scheduling and communication layer, primarily used for Charm++. 
This is a clean reimplementation of Converse without the complexities and deadcode that had accumulated in the old Converse. It is a complete task-based runtime system, that can be used in a stand-alone mode (i.e without Charm++), and with support for message-driven scheduling, multiple queues, user-level threads and communication. It schedules execution on CPUs as well as GPGPUs.   

## Documentation

- [Reconverse on Windows](README.windows.md): building and testing a single-process Reconverse with MSVC.
- [Reconverse internals](doc/internals/README.md): how the runtime works inside, one chapter per subsystem. Chapters so far:
  - [Shared-memory IPC](doc/internals/shared-memory-ipc.md): how the `+ipc` pool works: segments and blocks, its lists and why they need no lock, the send, receive and free paths, message order, memory use and start-up.
- [Converse and Reconverse](doc/converse-and-reconverse.md): how reconverse differs from classic Converse, with a block diagram of the modules and their interfaces, a side-by-side comparison, API compatibility notes, and what returns for AMPI (isomalloc, migratable threads).

## Build Reconverse

### Build Reconverse with Single-node Support

If you want to run Reconverse locally (single node), all you have to do is the following:  

```
$ cd reconverse
$ mkdir build
$ cd build
$ cmake ..
$ make
```

##### CMake Configure options
You can configure the build by passing options to CMake with:

```bash
cmake -D<option_name>=<value> ..
```

Some general options include:

* **`RECONVERSE_ENABLE_CPU_AFFINITY`** (`ON` by default if HWLOC is found)
  Enables CPU affinity support via [hwloc](https://www.open-mpi.org/projects/hwloc/). Requires HWLOC to be installed.

* **`RECONVERSE_ENABLE_XPMEM`** (`OFF` by default)
  Builds the XPMEM backend for shared-memory IPC in addition to the POSIX
  shared memory one, so that `+ipcmode xpmem` can use it (`auto` never picks
  it; see [Mechanisms](#mechanisms)). Requires an XPMEM installation;
  point CMake at it with `-DXPMEM_ROOT=<prefix>` if it is not in `/opt/xpmem`.
  The `-xpmem` ctest entries it adds need `/dev/xpmem` where the tests run.

* **`RECONVERSE_USE_JEMALLOC`** (`OFF` by default)
  Links jemalloc's `libjemalloc_pic.a` into libreconverse (whole-archive), so
  jemalloc's `malloc`/`free` replace libc's for the whole process; no change is
  needed in Charm++ or the application. Messages are allocated on one PE and
  freed on another, which glibc's malloc handles slowly with many threads
  (reconverse#271). Point CMake at the install with `-DJEMALLOC_ROOT=<prefix>`.
  Build jemalloc 5.4.0 from the release tarball with
  `./configure --prefix=<prefix> && make -j && make install`; the default build
  produces `libjemalloc_pic.a`. jemalloc keeps its metadata out of line, so the
  header reconverse puts in front of each message is unaffected. Startup prints
  `Reconverse> allocator: jemalloc` when this is on.

* **`CMAKE_BUILD_TYPE`** (not set by default)
  Selects the build type and corresponding compiler flags. For example:

  * `Release`: compiles with `-O3`
  * `Debug`: compiles with `-g`

### Build Reconverse with Multi-node Support

Currently, Reconverse has two communication backends:
- LCI (https://github.com/uiuc-hpc/lci): the preferred backend for Infiniband, RoCE, and Slingshot-11 clusters. It is expected to achieve better performance than MPI.
- LCW (https://github.com/JiakunYan/lcw): the fallback backend for traditional MPI clusters. It is compatible with a wide range of hardware but may not achieve the same performance as LCI. LCW is merely a active message wrapper layer for MPI.

##### LCI Backend Options

* **`RECONVERSE_TRY_ENABLE_COMM_LCI2`** (`ON` by default)
  Attempts to find an external LCI installation and enable the LCI backend.

* **`RECONVERSE_AUTOFETCH_LCI2`** (`OFF` by default)
  Automatically fetches LCI from GitHub if no external installation is found.

* **`RECONVERSE_AUTOFETCH_LCI2_TAG`** (defaults to a predefined commit hash)
  Specifies the Git commit hash, tag, or branch to use when fetching LCI.

* **`FETCHCONTENT_SOURCE_DIR_LCI`** (not set by default)
  Path to a local LCI source tree. If set, autofetch uses this local copy instead of fetching from GitHub.

If LCI is autofetched, you can further customize the LCI build by passing additional CMake variables. Important ones include
- **`-DLCI_NETWORK_BACKENDS=[ofi|ibv]`** (`ibv;ofi` by default): explicitly select the LCI backend to be libfabric (ofi) or libibverbs (ibv). `ibv` should be used for Infiniband and RoCE clusters. `ofi` should be used for shared memory system (e.g. laptop) and slingshot-11 clusters.
- **`-DLCT_PMI_BACKEND_ENABLE_MPI=ON`** (Default: `OFF`): let LCI bootstrap with MPI.
- **`-DLCI_WITH_SHM=ON`** (Default: `OFF`): use POSIX shared memory communication through LCI. This is only enabled for small messages (data size 108 bytes or less)

##### LCW (MPI) Backend Options

* **`RECONVERSE_TRY_ENABLE_COMM_LCW`** (`ON` by default)
  Attempts to find an external LCW installation and enable the LCW backend.

* **`RECONVERSE_AUTOFETCH_LCW`** (`OFF` by default)
  Automatically fetches LCW from GitHub if no external installation is found.

* **`RECONVERSE_AUTOFETCH_LCW_TAG`** (defaults to a predefined commit hash)
  Specifies the Git commit hash, tag, or branch to use when fetching LCW.

* **`FETCHCONTENT_SOURCE_DIR_LCW`** (not set by default)
  Path to a local LCW source tree. If set, autofetch uses this local copy instead of fetching from GitHub.

## Run Reconverse

The example executables are located in the build/test/<program_name> folders. You can run them with `mpirun`, `srun`, or `lcrun` depending on your system configuration.

A single process can also be started directly (`./program +pe 4`), also in a
shell inside a Slurm job, such as a batch script or an interactive session.
With an LCI older than 8d0be6b (uiuc-hpc/lci#223), such as a separately
installed one, a process started that way waits at startup forever: LCI's
bootstrap took the job's `SLURM_NTASKS` to mean that many processes had been
launched without a PMI service. With such an LCI, start it with `srun -n 1`, or
set `LCT_PMI_BACKEND=local`.

### Runtime Options
- **`+pe <num>`**: specify the total number of PEs across all processes.
- **`+thread_queue_max <num>`**: with `+old-scheduler`, after this many consecutive messages taken from the PE's thread queue (the queue other PEs and the network push into), one message from the PE's priority queue runs if there is one, so a stream from outside cannot starve the PE's own queued work (default 16).
- **`+node_queue_max <num>`**: the default scheduler runs node-level messages (the node queue, and node priority queue messages whose priority is at least as high as the PE's own next message) ahead of the PE's own queued work, and `+old-scheduler` runs the node queue ahead of it and takes the node priority queue whenever its lock is free; after this many node-level messages in a row, one of the PE's own messages runs if there is one, so a stream of node messages cannot starve it (default 16).
- **`+backend <lci|lcw>`**: select the communication backend at runtime. If not specified, Reconverse will use the first available backend in the order of LCI, LCW.
- **`+ipc`** / **`+noipc`**: send messages between processes that share a host through shared memory instead of the communication backend. Off by default; see [Shared-memory IPC](#shared-memory-ipc-ipc).
- **`+ipcmode <auto|shm|xpmem>`**: pick the mechanism backing the shared-memory pool. Implies `+ipc`. `auto`, the default, is `shm`.
- **`++ipcpoolsize <bytes>`**: size of each process's shared-memory pool. By default, 512 KiB for each PE of the host's other processes, and at least 8 MiB (see [Memory use](doc/internals/shared-memory-ipc.md#memory-use)).
- **`++ipccutoff <bytes>`**: largest message the pool will carry. Messages above it go over the communication backend. Defaults to 32 KiB, or to a bin below `poolsize / 25` when that is smaller. At most 1 MiB; a larger value is capped there, with a notice.
- **`+randomized_msgq`**: run messages in a uniformly random order, for shaking out message-order races. See [Randomized message order](#randomized-message-order-randomized_msgq). Cannot be combined with `+old-scheduler`.
- **`+randomized_seed <N>`**: base seed for `+randomized_msgq`. Defaults to a value taken from the wall clock; the startup banner prints the seed in use.
- **`+glibc_tuned_malloc`**: on glibc systems, raise glibc's malloc thresholds at startup (`mallopt`: mmap threshold 32 MB, trim threshold 1 GB, top pad 256 MB) and print `Reconverse> glibc malloc (+glibc_tuned_malloc): ...`. Off by default; accepted and ignored on other C libraries. See [glibc malloc tuning](#glibc-malloc-tuning-glibc_tuned_malloc).
- **`+nomempool_remote_free`**: with the LCI backend, a PE that frees a message from another PE's pool (a receive buffer another thread polled in, or a message another PE allocated) takes that pool's lock, as before. By default it instead pushes the block on a lock-free stack that the owner empties under its own lock the next time it allocates or frees one of its own blocks. `+mempool_remote_free`, the flag that turned this on before it was the default, is still accepted.
- **`+lci_max_rdv_sends <num>`**: with the LCI backend, the most messages above LCI's buffer-copy limit (8116 bytes at the default 8 KiB packet) that a process keeps in flight. Each such message goes by LCI's rendezvous protocol and holds a packet of the pool receives are posted from until it completes; without a limit, a process sending many of them at once could take every packet and hang, unable to receive the replies that would release them (issue #257). Further ones wait in the queue described under [Sends that LCI cannot take at once](#sends-that-lci-cannot-take-at-once), and the PE carries on. Default: half of the LCI packets (`LCI_ATTR_NPACKETS`) that the preposted receives do not take, 30720 with one device and the default pool.
- **`+lci_post_tries <num>`**: with the LCI backend, how many times a send is offered to LCI, with a poll of the network in between, before it is queued (default 1). See [Sends that LCI cannot take at once](#sends-that-lci-cannot-take-at-once).

### Sends that LCI cannot take at once

With the LCI backend a send (`CmiSyncSendAndFree` and the other send calls, and the zero-copy get and put) returns whether or not the network can take the message now. LCI refuses an operation when it has no room for it: its send queue or packet pool is used up, typically because the destination process is not polling the network (it is in a long handler, or blocked inside another library such as an MPI collective). The backend offers the operation to LCI `+lci_post_tries` times; if LCI still refuses, the backend keeps the operation in a queue for that destination process, and the PE goes on with its work. Whenever a PE of the process polls the network (every scheduler loop does, and so does every send), queued operations are offered again, oldest first for each destination.

- **Order.** An operation for a destination that has queued operations joins the queue behind them, so messages one PE sends to one destination process are handed to the network in the order they were sent. A destination that does not drain delays only its own queue.
- **Memory.** The queues have no size limit. A queued message is the buffer the sender allocated and nothing more (it is not copied), so a PE that keeps sending to a process that does not receive accumulates its own unsent messages; nothing slows the sender down. This is what an MPI program sees with `MPI_Isend`.
- **Exit.** `ConverseExit` hands the queued operations to the network before the processes synchronize to exit. If a destination takes nothing for 60 seconds, the process prints how many operations and bytes remain and for which destinations, drops them, and goes on exiting. `CmiAbort` does not try to send them.
- **A PE that never returns to its scheduler** after sending (and makes no further send) leaves its queued messages unsent until some PE of its process polls.

### glibc malloc tuning (`+glibc_tuned_malloc`)

Messages are allocated by one thread and freed by another. With glibc's
default thresholds, freed message memory is returned to the OS (munmap of
large blocks, trimming of heap tops) and faulted back in on the next
allocation, and with many PEs in one process the faults and munmaps serialize
on the process's memory-map lock. `+glibc_tuned_malloc` keeps that memory in
the process instead. It is opt-in because the retained memory raises the
resident set. Measured on Anvil, 2 nodes, one process per node, against
reconverse main without the flag:

| Application | Speedup | Peak RSS |
|---|---|---|
| FoF3 | 1.65x | 1.07x |
| ChaNGa | 1.31x | 1.05x |
| Task Bench | 1.14x | 1.02x |
| SSSP | 1.36x | 2.19x |

A `GLIBC_TUNABLES` setting containing `glibc.malloc.` takes precedence: the
`mallopt` calls are skipped and startup says so.

## Shared-memory IPC (`+ipc`)

Two processes on the same host do not need the network to talk to each other.
With `+ipc`, every process maps a pool of shared memory into each of its
peers' address spaces, and `CmiSyncSend*`/`CmiSyncNodeSend*` to a peer process
on the same host copy the message straight into that peer's pool instead of
handing it to LCI or MPI. Nothing about the messaging API changes: a message
that arrives through the pool is indistinguishable from one off the network,
down to the destination field in its header, and the same handler runs.

This is off by default, so a run that does not ask for it behaves exactly as
before. Turn it on with `+ipc`:

```
$ srun -n 4 ./reconverse_ping_ack +pe 8 +ipc
```

Messages fall back to the communication backend, per message, whenever the
pool cannot carry them: the destination is on another host, the message is
larger than `++ipccutoff`, or the destination's pool is full. A program
therefore never has to know whether IPC is on.

A pool fills for good, not for a moment. A freed block goes back only to the
PE that allocated it, so once a process's pool has been cut into blocks, a
sender with no freed block of the right size there uses the network for its
messages to that process. The first such send in a process prints a notice,
and `CmiIpcPoolFullSends` counts them; `++ipcpoolsize` raises the size. See
[Memory use](doc/internals/shared-memory-ipc.md#memory-use).

### Mechanisms

* **`shm`** — POSIX shared memory (`shm_open`/`mmap`). Needs nothing beyond a
  working `/dev/shm`, so it is the fallback everywhere.
* **`xpmem`** — [XPMEM](https://github.com/hpc/xpmem), which maps one process's
  private memory into another's, avoiding the shm file entirely. Must be built
  in with `-DRECONVERSE_ENABLE_XPMEM=ON` (point CMake at a non-standard
  install with `-DXPMEM_ROOT=<prefix>`), and needs the `xpmem` kernel module
  loaded at run time. Asking for it on a host without `/dev/xpmem` is an
  error rather than a silent fallback.
* **`auto`** (the default) — `shm`, even where xpmem is built in and
  available. On Frontier xpmem was never faster than `shm`, and with the two
  processes on different NUMA domains it was 1.3x slower at 2 KiB and
  1.5-1.7x slower from 8 KiB up (see the Frontier table below). Both copy a message once, but they put the pages
  in different places. A POSIX shm page goes to the NUMA domain of the process
  that first writes it, and for a block that is the sender, so the sender's
  copy stays local. Every page of an xpmem segment goes to its owner's domain,
  the receiver, so every send writes to memory on another domain.

### What it buys

`tests/orig-converse/pingpong`, two processes with one PE each on one NCSA
Delta CPU node, LCI over Slingshot-11 as the backend, 1000 round trips per
size (one-way microseconds):

| message | backend | `+ipc` | speedup |
| ------: | ------: | -----: | ------: |
|     8 B |    3.42 |   0.76 |    4.5x |
|    32 B |    3.21 |   0.77 |    4.2x |
|   128 B |    3.72 |   0.75 |    4.9x |
|   512 B |    3.86 |   0.76 |    5.1x |
|    2 KiB |   4.38 |   1.01 |    4.4x |
|    8 KiB |  14.86 |   2.26 |    6.6x |
|   32 KiB |  15.39 |   6.53 |    2.4x |
|  128 KiB |  21.07 |  24.65 |    0.85x |

The pool wins by a wide margin up to tens of kilobytes and loses past that,
and what it loses to is its own copy. Carrying a message costs one memcpy:
the sender copies it into a block in the destination's segment, and the
receiver takes delivery of that block where it lies. That cost grows with the
message. The backend's does not, in the same way -- the jump from 4.38 to
14.86 us between 2 and 8 KiB is LCI switching from eager to rendezvous, and
past that point it transfers out of the registered mempool `CmiAlloc` already
hands it, so the bytes move by DMA with no CPU copying them. Once a message
is large enough, one memcpy of it costs more than LCI's fixed handshake.

It is *not* that LCI's on-host path is itself shared memory. If it were, 8
bytes would not cost 3.42 us; that is not the price of a memcpy.

The default cutoff is 32 KiB, the largest size measured here at which the
pool still wins, so a default run takes the slower path for nothing in this
table. That makes it a conservative floor rather than a tuned value: the
crossover is somewhere between 32 and 128 KiB here, and elsewhere on another
machine or backend, so a program that moves a lot of 64-128 KiB messages
between processes on a host may do better with `++ipccutoff 131072`. Measure
before raising it.

The same pingpong on one OLCF Frontier node (AMD EPYC 7A53, four NUMA
domains of 16 cores, eight cores to an L3), LCI over Slingshot-11
(CXI) as the backend, `++ipccutoff 1048576` so that the pool carries every
size, one-way microseconds, median of three runs of 10,000 round trips. The
backend's time does not depend on where the two processes sit.

| message | backend | `shm`, same L3 | `shm`, same NUMA domain | `shm`, other NUMA domain | `xpmem`, other NUMA domain |
| ------: | ------: | -----: | -----: | -----: | -----: |
|     8 B |    2.63 |   0.39 |   0.72 |   0.78 |   0.77 |
|   512 B |    3.34 |   0.42 |   0.74 |   0.86 |   0.86 |
|   2 KiB |    3.71 |   0.46 |   1.02 |   1.11 |   1.42 |
|   8 KiB |   14.41 |   0.83 |   2.02 |   2.22 |   3.29 |
|  32 KiB |   16.40 |   2.07 |   5.11 |   6.02 |   9.34 |
| 128 KiB |   27.67 |   6.61 |  17.81 |  21.26 |  31.29 |
| 256 KiB |   43.36 |  12.57 |  35.23 |  39.94 |  66.23 |
| 512 KiB |   75.15 |  18.17 |  68.89 |  82.35 | 132.20 |

(Cores 1 and 2, 1 and 9, and 1 and 57.) `xpmem` was within 5% of `shm` at
every size in the first two placements, so only its NUMA-crossing column is
shown. The same-L3 column is a best case: the pingpong reuses one block per
size, which stays in the shared cache.

With `shm` the pool beats the backend at every size up to 256 KiB wherever
the two processes sit. Across NUMA domains the crossover lies between 256 and
512 KiB, and within one domain it lies above 512 KiB. That is well past
Delta's, so on Frontier the 32 KiB default sends messages above 32 KiB and up
to 256 KiB over the backend although the pool would be 1.1x (256 KiB, other
NUMA domain) to 6x (64 KiB, same L3) faster, and `++ipccutoff 262144` is
worth measuring for a program that sends many of them. The default stays at 32 KiB because it
has to be safe on Delta, where the pool already loses at 128 KiB.

### Querying it from a program

```c
int CmiIpcEnabled(void);          /* is the pool up? */
const char *CmiIpcImplName(void); /* "posixshm", "xpmem", or "none" */
int CmiIpcNumPeers(void);         /* processes on this host, this one included */
long CmiIpcMessagesSent(void);    /* messages this PE put into the pool */
long CmiIpcMessagesReceived(void);/* messages this PE took out of it */
long CmiIpcPoolFullSends(void);   /* sends this PE made over the network
                                     because the destination's pool was full */
size_t CmiIpcPoolBytesUsed(void); /* bytes of this process's pool cut into
                                     blocks so far */
```

`tests/ipc` uses these to check that traffic really took the pool.

### Ordering a notification behind a one-sided put

A put issued through the communication backend is still in flight when it
completes locally: local completion says the source buffer may be reused, not
that the bytes are visible in the destination process. A notification sent
straight after it -- "your buffer is filled" -- goes through the pool when the
destination is a peer process on this host, and the pool does not wait for the
network, so the notification can overtake the data it is announcing.

Reconverse closes this inside the runtime for the two places that put: the
Converse zerocopy Direct API keeps a put's acknowledgement on the backend
(`CommRputLocalHandler`), and a persistent channel to a pool peer sends the
payload inline rather than putting at all (`writeToBuffer`). A layer that
issues its own puts has the same problem, and these to fix it with:

```c
void CmiIpcBeginNetworkOnly(void); /* sends from this PE stay on the backend */
void CmiIpcEndNetworkOnly(void);   /* ...until here; nestable */
bool CmiIpcReaches(int destNode);  /* would the pool carry a message there? */
```

`tests/rdma_ipc_ack` covers the Direct API case from both sides: that the
acknowledgement is kept off the pool, and that the destination's buffer really
holds the data by the time the acknowledgement is delivered.

### Caveats

* `+ipc` makes Reconverse call `CmiInitCPUTopology` during startup, because
  the pool has to know which processes share a host. That adds a small
  collective to startup and prints the usual topology line.
* Cross-partition sends (`CmiInterSyncSend` and friends) always use the
  communication backend.
* Messages from one PE to another that both go through the pool arrive in the
  order sent, with any number of PEs per process: each PE has its own receive
  queue and is the only one that takes from it (`tests/ipc_order` checks
  this). But a message under the cutoff and one over it travel by different
  routes, so they can cross, and so can a message sent while the destination's
  pool is full and one sent after. Converse has never ordered messages
  between PEs, so none of this breaks a guarantee, but a program that happened
  to rely on the ordering the network backend gave it will notice. A one-sided
  put is reordered against pool messages the same way, and there it *can*
  break a program: see [Ordering a notification behind a one-sided
  put](#ordering-a-notification-behind-a-one-sided-put).
* When a host runs more than one process, each may have at most 512 PEs and
  the host at most 1,024 (`kMaxIpcRanks`, `kMaxIpcHostPes`); a larger run
  stops at start-up with an error. `++ipccutoff` is at most 1 MiB.
* A process killed outright (not `CmiExit`) leaves its POSIX shared memory
  segment behind in `/dev/shm`; a clean exit unlinks it. `xpmem` has nothing
  to leave behind.

## Randomized message order (`+randomized_msgq`)

A debugging mode. With `+randomized_msgq`, each PE's scheduler loop moves
every message in its own sources into one pool before running anything:
the converse thread queue, the PE priority queue (self-sends included),
messages the network just delivered and blocks a peer process left in the
shared-memory IPC pool. From the sources it shares with the other PEs (the
node queue, the node priority queue and the task queue with
`CMK_TASKQUEUE`) it takes one message per iteration, so the rest stay where
the other PEs can run or steal them at the same time, as under the default
loop. It then runs one message chosen uniformly at random from the pool,
and repeats. Nothing bypasses the pool, so priorities, FIFO order and
Charm++'s `[expedited]` entries are not respected, and a program that
depends on message order without guaranteeing it tends to fail quickly.

```
$ ./reconverse_megarecon +pe 4 +randomized_msgq
Reconverse> Randomized message queue (+randomized_msgq, seed 1791586969496906): priorities, FIFO order and [expedited] are not respected.
```

Each PE seeds its own `std::mt19937_64` from the base seed and its PE
number, and reduces its output to a pool index itself, so a seed gives the
same draws with every compiler and standard library. Passing
`+randomized_seed <N>` repeats the sequence of draws; it does not make the
run deterministic, because message arrival timing still varies. Without
`+randomized_seed`, each process takes its own seed from its clock and the
banner shows process 0's, so pass the flag to repeat a multi-process run.

Charm++ under this mode (branch `recharm`, 2026-10-10): megatest
passes except its `priotest` module, which checks priority order; the
Charm++ CI test set and `sdag`, `load_balancing`, `reductionTesting`,
`chkpt` and `demand_creation` pass; `tests/charm++/delegation/multicast`
hangs on more than one PE, an ordering dependence in CkMulticast's
migration path that the mode found (charm #4032).

The mode is a separate scheduler loop chosen once when `CsdScheduler()` or
`CsdSchedulePoll()` is entered; the default loop is unchanged and costs
nothing extra when the flag is absent.

With the flag, the loop itself gets slow at many PEs per process. Each
iteration sizes the thread queue and checks it for emptiness before every
take, and both walk every producer that has pushed to it (#271), so at
64 PEs per process a backlog of N messages costs on the order of N µs per
iteration. That is expected in this mode; it goes away when #271 is fixed.

## Aborts and stack traces

`CmiAbort` prints its reason and the calling thread's stack (demangled) to
stderr, asks the process manager to end the other ranks, and aborts. The
process-manager step goes through LCI's `LCT_pmi_abort`: under `srun` with
`--mpi=pmi2` or `--mpi=pmix`, and under `mpirun`, the whole job ends at once;
`lcrun` ends the other processes itself when one exits with a failure; with
the file PMI backend (no launcher) nothing else is needed.

A program that owns the job can end it its own way: `CmiSetAbortHook(fn)`
registers a function that `CmiAbort` calls once per process, with code 1 and
the formatted reason, after printing and before the process-manager step.
If the hook returns, `CmiAbort` goes on as usual. Another PE that aborts
meanwhile waits for the hook, so that its `abort()` does not end the process
before the hook has ended the job, but for at most 10 s; the hook must not
take Converse locks, which an aborting PE may hold. Reconverse registers no
hook itself. MPI interop needs one, because there LCI bootstraps over LCT's
tcp backend, whose abort reaches no other process: Charm++'s `CharmLibInit`
registers a hook that calls `MPI_Abort` (charmplusplus/charm#4254).

`abort()` skips the process's exit handlers, and with them the clang profile
runtime's write of a coverage profile. `CmiRegisterAbortFlush(fn)` registers
an `int fn(void)` that `CmiAbort` calls before the hook; up to 8, run once per
process in the order registered, with other aborting PEs waiting for them as
for the hook. With `-DRECONVERSE_COVERAGE=ON` (set by Charm++'s `--coverage`)
`CmiAbort` first writes reconverse's own profile. Each image linked with the
profile runtime (libreconverse, the executable) has its own copy of it, so an
executable that is instrumented registers its own `__llvm_profile_write_file`;
Charm++ does this in coverage builds.

Fatal signals
(SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) print the PE, the signal and a
stack trace, then re-raise with the default disposition, so exit status and
core dumps are unchanged. `+truecrash` leaves those handlers uninstalled for
a debugger or a core dump that should see the fault first-hand. On Linux the
executable's own functions appear in the traces only when it is linked with
`-rdynamic` (reconverse's CMake interface adds it to its consumers; Charm++'s
charmc does for reconverse targets).

## CCS (Converse Client-Server)

CCS lets a program outside the parallel job send requests to handlers
registered inside it and read their replies over a TCP socket; Charm++'s
liveViz, PythonCCS and the CCS builtins use it. It is part of
libreconverse: `conv-ccs.h` for the server side (`CcsRegisterHandler`,
`CcsSendReply`, `CcsDelayReply`, `CcsSetMergeFn`), `ccs-client.h` plus the
`ccs-client` static library for the client side, which needs no Converse.
`ConverseInit` initializes CCS on every PE; a program registers its
handlers in its start function and never calls `CcsInit`.

Nothing listens unless the run is started with `++server` (an OS-chosen
port), `++server-port <port>` or `++server-auth <file>`. PE 0 then opens
the socket and prints `ccs: Server IP = ..., Server port = ...`, polls it
from the `CcdPERIODIC` condition, forwards each request to its PE as a
Converse message, and sends every reply. A request addressed to PE -1 is a
broadcast: every PE's reply is combined by the handler's merge function
(`CcsSetMergeFn`; `CcsMerge_concat` and the sum/max/min families are
provided) in a `CmiReduce`. A request with a PE list is a multicast,
combined the same way with `CmiListReduce` over that list, under the
reduction id `CcsSetMergeFn` took from `CmiGetGlobalReduction`.
`tests/ccs` runs all three through the client library.

Not carried over from classic Converse: CharmDebug's conditional delivery
and stdio forwarding, which depend on the Converse debugger.

There is no charmrun to relay requests, so the client must reach PE 0's
socket on its compute node. On a cluster whose compute nodes are reachable
from the login node but not from outside, forward the port through the
login node with ssh and point the client at localhost:

```
# job script: a fixed port, so the address is known
srun --mpi=pmix -n 2 ./app +pe 4 ++server ++server-port 1234
# job output: ccs: Server IP = <address>, Server port = 1234
# laptop:
ssh -L 1234:<compute node>:1234 <login node>
./client localhost 1234
```

Verified on NCSA Delta (2026-10-20): examples/converse/ccstest from Charm++
run under srun on a cpu-interactive node, client on a laptop through
`ssh -L`, replies from all four PEs. `++server-auth <file>` protects the
socket as before.

## Example Steps to Build and Run Reconverse

### Build and run Reconverse on your own laptop with autofetched LCI backend

#### Prerequisite:
- `libfabric` as LCI's network backend for shared memory system.
You can install them with
```
$ sudo apt install libfabric-bin libfabric-dev
```

#### Build reconverse
```
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_AUTOFETCH_LCI2=ON ..
$ make
```

#### Run reconverse
Using `lcrun` to run the reconverse example is typically the most simplest way. First, you need to locate LCI's `lcrun` executable. It is located in the LCI source directory and will be installed to the `bin` folder if you installed LCI by yourself. If you used the cmake autofetch support, it will typically be located in the `<build_directory>/_deps/lci-src` folder.

Run the reconverse example with `lcrun`:

```
$ cd build/
$ _deps/lci-src/lcrun -n 2 test/ping_ack/reconverse_ping_ack +pe 4
```

**Note:** if you installed `libfabric` in a non-standard location, the linker *may* complain it cannot find the libfabric shared library, in which case you need to let the linker find them by
```
export LD_LIBRARY_PATH=<path_to_libfabric_lib>:${LD_LIBRARY_PATH}
```

### Build and run Reconverse on NCSA Delta with autofetched LCI backend

To use CMake Autofetch support:
```
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_AUTOFETCH_LCI2=ON -DLCI_NETWORK_BACKENDS=ofi ..
$ make
```
Note: Explicitly specifying `-DLCI_NETWORK_BACKENDS=ofi` is only needed for Slingshot-11 systems.

Run the reconverse example with `srun`:

```
$ cd build/
$ srun --mpi=pmi2 -n 2 test/ping_ack/reconverse_ping_ack +pe 4
```

### Build and run Reconverse on NCSA Delta with your own LCI installation

If you want to install LCI by yourself, here is an example build procedure on NCSA's Delta machine using the OFI layer:

```
$ git clone https://github.com/uiuc-hpc/lci.git --branch=lci2
$ cd lci
$ export OFI_ROOT=/opt/cray/libfabric/1.15.2.0
$ export LCI_ROOT=/where/you/want/to/install/lci
$ cmake -DCMAKE_INSTALL_PREFIX=$LCI_ROOT .
$ make install
$ cd ..
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build && cd build
$ cmake ..
$ make
```

Run the reconverse example with `srun`:

```
$ cd build/test/pingpong
$ srun -n 2 ./reconverse_ping_ack +pe 4
```

### Build and run Reconverse with MPI

Make sure you have an MPI implementation installed (e.g., OpenMPI, MPICH, etc.). Then, to build Reconverse with the LCW backend using CMake Autofetch support:

```
$ export MPI_ROOT=/path/to/mpi  # if not installed in a standard location
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_AUTOFETCH_LCW=ON ..
$ make
```

Run the reconverse example with `srun` or `mpirun`:

```
$ cd build/
# `+backend lcw` is optional if you only have the LCW backend
$ mpirun -n 2 test/ping_ack/reconverse_ping_ack +backend lcw +pe 4
```
