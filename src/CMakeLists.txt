target_include_directories(reconverse PRIVATE .)

# Seed load-balancing strategy: compiles cldb.${RECONVERSE_CLDB}.cpp
set(RECONVERSE_CLDB "neighborhood" CACHE STRING
    "Seed LB strategy (rand | neighborhood)")

target_sources(reconverse PRIVATE conv-conds.cpp convcore.cpp random.cpp
                scheduler.cpp scheduler_helpers.cpp cpuaffinity.cpp mempool.C collectives.cpp
                comm_backend/comm_backend_internal.cpp threads.cpp cldb.${RECONVERSE_CLDB}.cpp cldb.cpp cmirdmautils.cpp
                conv-rdma.cpp conv-topology.cpp msgmgr.cpp queueing.cpp cmishmem.cpp
                conv-taskq.cpp persist-comm.cpp)
target_include_directories(
  reconverse PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
                     
add_subdirectory(comm_backend)

# configure needs to be called after add_subdirectory(comm_backend)
configure_file(converse_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/converse_config.h)
