# reconverse-pingpong
Basic implementation of a new communication layer for Charm++

## Install and run

If you want to run Reconverse locally (single node), all you have to do is the following:  

```
$ cd reconverse
$ mkdir build
$ cd build
$ cmake -DRECONVERSE_TRY_ENABLE_COMM_LCI1=OFF ..
$ make
```

If you want to run Reconverse in multi-node mode, you first need to install LCI (https://github.com/uiuc-hpc/lci). Note that multi-node is currently only supported on Linux-based systems.  
Building multi-node Reconverse may be different based on your platform. Here is an example build procedure on NCSA's Delta machine using the OFI layer:  

```
$ git clone https://github.com/uiuc-hpc/lci.git
$ cd lci
$ export CMAKE_INSTALL_PREFIX=/u/<username> (or somewhere else you prefer)
$ export OFI_ROOT=/opt/cray/libfabric/1.15.2.0
$ cmake -DLCI_SERVER=ofi .
$ make intall
$ cd ..
$ git clone https://github.com/charmplusplus/reconverse.git
$ cd reconverse
$ mkdir build && cd build
$ cmake ..
$ make
```

In the build/examples/<program_name> folder, run the `reconverse_<program_name>` executable. Currently, the first arguments must be `+pe <num_pes>`.  
