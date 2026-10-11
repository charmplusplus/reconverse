# reconverse
Complete an mostly backward compatible implementation of a new scheduling and communication layer, primarily used for Charm++. 
This is a clean reimplementation of Converse without the complexities and deadcode that had accumulated in the old Converse. It is a complete task-based runtime system, that can be used in a stand-alone mode (i.e without Charm++), and with support for message-driven scheduling, multiple queues, user-level threads and communication. It schedules execution on CPUs as well as GPGPUs.   

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
  shared memory one, so that `+ipcmode xpmem` (and `+ipcmode auto` on a host
  with the kernel module loaded) can use it. Requires an XPMEM installation;
  point CMake at it with `-DXPMEM_ROOT=<prefix>` if it is not in `/opt/xpmem`.

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

### Runtime Options
- **`+pe <num>`**: specify the total number of PEs across all processes.
- **`+backend <lci|lcw>`**: select the communication backend at runtime. If not specified, Reconverse will use the first available backend in the order of LCI, LCW.
- **`+ipc`** / **`+noipc`**: send messages between processes that share a host through shared memory instead of the communication backend. Off by default; see [Shared-memory IPC](#shared-memory-ipc-ipc).
- **`+ipcmode <auto|shm|xpmem>`**: pick the mechanism backing the shared-memory pool. Implies `+ipc`.
- **`++ipcpoolsize <bytes>`**: size of each process's shared-memory pool (8 MiB by default).
- **`++ipccutoff <bytes>`**: largest message the pool will carry. Messages above it go over the communication backend. Defaults to 32 KiB, or to a bin below `poolsize / 25` when that is smaller.
- **`+randomized_msgq`**: run messages in a uniformly random order, for shaking out message-order races. See [Randomized message order](#randomized-message-order-randomized_msgq). Cannot be combined with `+old-scheduler`.
- **`+randomized_seed <N>`**: base seed for `+randomized_msgq`. Defaults to a value taken from the wall clock; the startup banner prints the seed in use.

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

Messages fall back to the communication backend, silently and per message,
whenever the pool cannot carry them: the destination is on another host, the
message is larger than `++ipccutoff`, or the pool is momentarily full. A
program therefore never has to know whether IPC is on.

### Mechanisms

* **`shm`** — POSIX shared memory (`shm_open`/`mmap`). Needs nothing beyond a
  working `/dev/shm`, so it is the fallback everywhere.
* **`xpmem`** — [XPMEM](https://github.com/hpc/xpmem), which maps one process's
  ordinary heap into another's, avoiding the shm file entirely. Must be built
  in with `-DRECONVERSE_ENABLE_XPMEM=ON` (point CMake at a non-standard
  install with `-DXPMEM_ROOT=<prefix>`), and needs the `xpmem` kernel module
  loaded at run time.
* **`auto`** (the default) — xpmem if it was built in *and* `/dev/xpmem` is
  present, otherwise POSIX shared memory. Asking for `+ipcmode xpmem`
  explicitly on a host without it is an error rather than a silent fallback.

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

### Querying it from a program

```c
int CmiIpcEnabled(void);          /* is the pool up? */
const char *CmiIpcImplName(void); /* "posixshm", "xpmem", or "none" */
int CmiIpcNumPeers(void);         /* processes on this host, this one included */
long CmiIpcMessagesSent(void);    /* messages this PE put into the pool */
long CmiIpcMessagesReceived(void);/* messages this PE took out of it */
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
* Messages to one destination can be reordered relative to each other, in two
  ways. A message under the cutoff and one over it travel by different routes,
  so they can cross. And a process has one receive queue, which every PE in it
  drains, so when a process runs several PEs a block taken by a PE other than
  the destination reaches that destination by a second hop and can arrive
  after a block taken later. What does not happen is wholesale reversal: the
  queue is FIFO, so a burst that goes entirely through the pool, from one PE
  to a process running one PE, arrives in the order it was sent
  (`tests/ipc_order` checks this), and no message has to wait for the queue to
  drain before anything looks at it. Converse has never ordered messages
  between PEs, so none of this breaks a guarantee, but a program that happened
  to rely on the ordering the network backend gave it will notice. A one-sided
  put is reordered against pool messages the same way, and there it *can*
  break a program: see [Ordering a notification behind a one-sided
  put](#ordering-a-notification-behind-a-one-sided-put).
* A process killed outright (not `CmiExit`) leaves its POSIX shared memory
  segment behind in `/dev/shm`; a clean exit unlinks it. `xpmem` has nothing
  to leave behind.

## Randomized message order (`+randomized_msgq`)

A debugging mode. With `+randomized_msgq`, each PE's scheduler loop moves
every message in its own sources into one pool before running anything:
the converse thread queue, the self queue, the PE priority queue, messages
the network just delivered and blocks a peer process left in the
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

Charm++ under this mode (`reviewed-with-reconverse`, 2026-10-10): megatest
passes except its `priotest` module, which checks priority order; the
Charm++ CI test set and `sdag`, `load_balancing`, `reductionTesting`,
`chkpt` and `demand_creation` pass; `tests/charm++/delegation/multicast`
hangs on more than one PE, an ordering dependence in CkMulticast's
migration path that the mode found (charm #4032).

The mode is a separate scheduler loop chosen once when `CsdScheduler()` or
`CsdSchedulePoll()` is entered; the default loop is unchanged and costs
nothing extra when the flag is absent.

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
