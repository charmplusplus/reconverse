# reconverse
Basic implementation of a new communication layer for Charm++

## Build Reconverse

If you want to run Reconverse locally (single node), all you have to do is the following:  

```
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI2=OFF ..
$ make
```

## Runtime options
- -DRECONVERSE_ENABLE_CPU_AFFINITY (ON by default if hwloc is found): Enable setting CPU affinity with HWLOC (must have HWLOC installed)
- -DCMAKE_BUILD_TYPE (not set by default): Set the build type to change which flags are passed to the compiler, e.g. use `Release` to compile with `-O3` or `Debug` to compile with `-g`

## LCI

Currently, Reconverse multi-node support is based on LCI (https://github.com/uiuc-hpc/lci). You could either install LCI by your own or use the cmake autofetch support.

To use the cmake autofetch support:
```
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI2=ON -DRECONVERSE_AUTOFETCH_LCI2=ON ..
$ make
```

Additional cmake variable can be passed to further fine-tune the build of LCI. Useful ones include
- `-DLCI_NETWORK_BACKENDS=[ofi|ibv]`: explicitly select the LCI backend to be libfabric (ofi) or libibverbs (ibv). `ibv` should be used for Infiniband and RoCE clusters. `ofi` should be used for shared memory system (e.g. laptop) and slingshot-11 clusters.
- `-DLCT_PMI_BACKEND_ENABLE_MPI=ON` (Default: `OFF`): let LCI bootstrap with MPI. This can be useful when the running environment does not have PMI support and `lcrun` becomes slow.

Note: LCI by default will automatically probe and select available network backends, but this procedure sometimes leads to unsatifactory results (e.g. on Delta where libibverbs is installed but no Infiniband devices available). 

### Run Reconverse

In the build/test/<program_name> folder, run the `reconverse_<program_name>` executable. Currently, the first arguments must be `+pe <num_pes>`.  

### Build and run Reconverse on your own laptop

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
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI2=ON -DRECONVERSE_AUTOFETCH_LCI2=ON -DLCI_NETWORK_BACKENDS=ofi ..
$ make
```

#### Run reconverse
Using `lcrun` to run the reconverse example is typically the most simplest way. First, you need to locate LCI's `lcrun` executable. It is located in the LCI source directory and will be installed to the `bin` folder if you installed LCI by yourself. If you used the cmake autofetch support, it will typically be located in the `<build_directory>/_deps/lci-src` folder.

Then, run the reconverse example with `lcrun`:

```
$ cd build/test/pingpong
$ lcrun -n 2 ./reconverse_ping_ack +pe 4
```

**Note:** if you installed `libfabric` in a non-standard location, the linker *may* complain it cannot find the libfabric shared library, in which case you need to let the linker find them by
```
export LD_LIBRARY_PATH=<path_to_libfabric_lib>:${LD_LIBRARY_PATH}
```

### Build and run Reconverse on NCSA Delta

#### Build reconverse
To use CMake Autofetch support:
```
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI2=ON -DRECONVERSE_AUTOFETCH_LCI2=ON ..
$ make
```

Note: NCSA has built-in PMI support. LCI will automatically detect and use it.

If you want to install LCI by yourself, here is an example build procedure on NCSA's Delta machine using the OFI layer:

```
$ git clone https://github.com/uiuc-hpc/lci.git --branch=lci2
$ cd lci
$ export CMAKE_INSTALL_PREFIX=/u/<username>/opt (or somewhere else you prefer)
$ export OFI_ROOT=/opt/cray/libfabric/1.15.2.0
$ cmake .
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
$ cd build/test/pingpong
$ srun -n 2 ./reconverse_ping_ack +pe 4
```
