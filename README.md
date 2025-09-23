# reconverse
Basic implementation of a new communication layer for Charm++

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
