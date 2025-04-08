# reconverse-pingpong
Basic implementation of a new communication layer for Charm++

## Build Reconverse

If you want to run Reconverse locally (single node), all you have to do is the following:  

```
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI1=OFF ..
$ make
```

## Runtime options
- -DENABLE_CPU_AFFINITY (off by default): Enable setting CPU affinity with HWLOC (must have HWLOC installed)

## LCI

Currently, Reconverse multi-node support is based on LCI (https://github.com/uiuc-hpc/lci). You could either install LCI by your own or use the cmake autofetch support.

To use the cmake autofetch support:
```
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI1=ON -DRECONVERSE_AUTOFETCH_LCI1=ON ..
$ make
```

Additional cmake variable can be passed to further fine-tune the build of LCI. Useful ones include
- `-DLCT_PMI_BACKEND_ENABLE_MPI=ON` (Default: `OFF`): let LCI bootstrap with MPI. This can be useful when the running environment does not have PMI support.
- `-DLCI_SERVER=[ofi|ibv]`: explicitly select the LCI backend to be libfabric (ofi) or libibverbs (ibv). `ibv` should be used for Infiniband and RoCE clusters. `ofi` should be used for shared memory system (e.g. laptop) and slingshot-11 clusters.

Note: LCI by default will automatically probe and select available network backends, but this procedure sometimes leads to unsatifactory results (e.g. on Delta where libibverbs is installed but no Infiniband devices available). 

### Run Reconverse

In the build/examples/<program_name> folder, run the `reconverse_<program_name>` executable. Currently, the first arguments must be `+pe <num_pes>`.  

### Build and run Reconverse on your own laptop

#### Prerequisite:
- `libfabric` as LCI's network backend for shared memory system.
- `mpi` to bootstrap LCI.
You can install them with
```
$ sudo apt install libfabric-bin libfabric-dev openmpi-bin openmpi-common openmpi-doc libopenmpi-dev
```

#### Build reconverse
```
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI1=ON -DRECONVERSE_AUTOFETCH_LCI1=ON -DLCI_SERVER=ofi -DLCT_PMI_BACKEND_ENABLE_MPI=ON ..
$ make
```

**Note:** if you installed `libfabric` or `mpi` in a non-standard location, CMake *may* complain it cannot find OFI/MPI, in which case you need to let CMake find them by
```
export OFI_ROOT=<path_to_libfabric>
export MPI_ROOT=<path_to_mpi>
```

#### Run reconverse
```
$ cd build/examples/pingpong
$ mpirun -n 2 ./reconverse_ping_ack +pe 4
```

**Note:** if you installed `libfabric` or `mpi` in a non-standard location, the linker *may* complain it cannot find the libfabric/mpi shared library, in which case you need to let the linker find them by
```
export LD_LIBRARY_PATH=<path_to_libfabric_lib>:<path_to_mpi_lib>:${LD_LIBRARY_PATH}
```

### Build and run Reconverse on NCSA Delta

#### Build reconverse
To use CMake Autofetch support:
```
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI1=ON -DRECONVERSE_AUTOFETCH_LCI1=ON -DLCI_SERVER=ofi ..
$ make
```

Note: NCSA has built-in PMI support. LCI will automatically detect and use it.

If you want to install LCI by yourself, here is an example build procedure on NCSA's Delta machine using the OFI layer:

```
$ git clone https://github.com/uiuc-hpc/lci.git
$ cd lci
$ export CMAKE_INSTALL_PREFIX=/u/<username> (or somewhere else you prefer)
$ export OFI_ROOT=/opt/cray/libfabric/1.15.2.0
$ cmake -DLCI_SERVER=ofi .
$ make install
$ cd ..
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build && cd build
$ cmake ..
$ make
```

#### Run reconverse
```
$ cd build/examples/pingpong
$ srun -n 2 ./reconverse_ping_ack +pe 4
```
