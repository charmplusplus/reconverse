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
- **`++ipccutoff <bytes>`**: largest message the pool will carry. Messages above it go over the communication backend. Defaults to a bin below `poolsize / 25`.

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

The pool wins by a wide margin up to tens of kilobytes and loses past that:
it copies the message into the destination's pool, while LCI's own on-host
path is already shared memory and does not. On this machine the crossover is
somewhere between 32 and 128 KiB, which is *below* the cutoff the pool picks
by default (256 KiB for the default 8 MiB pool), so a program that sends
large messages between processes on a host should measure and lower
`++ipccutoff` -- e.g. `++ipccutoff 65536`. Where the crossover falls depends
on the backend and the machine, so measure rather than copying this number.

### Querying it from a program

```c
int CmiIpcEnabled(void);          /* is the pool up? */
const char *CmiIpcImplName(void); /* "posixshm", "xpmem", or "none" */
int CmiIpcNumPeers(void);         /* processes on this host, this one included */
long CmiIpcMessagesSent(void);    /* messages this PE put into the pool */
long CmiIpcMessagesReceived(void);/* messages this PE took out of it */
```

`tests/ipc` uses these to check that traffic really took the pool.

### Caveats

* `+ipc` makes Reconverse call `CmiInitCPUTopology` during startup, because
  the pool has to know which processes share a host. That adds a small
  collective to startup and prints the usual topology line.
* Cross-partition sends (`CmiInterSyncSend` and friends) always use the
  communication backend.
* Messages to one destination can be reordered relative to each other, because
  a message under the cutoff and one over it travel by different routes.
  Converse has never ordered messages between PEs, so this breaks no
  guarantee, but a program that happened to rely on the ordering the network
  backend gave it will notice.
* A process killed outright (not `CmiExit`) leaves its POSIX shared memory
  segment behind in `/dev/shm`; a clean exit unlinks it. `xpmem` has nothing
  to leave behind.

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
