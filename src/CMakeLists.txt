target_include_directories(reconverse PRIVATE .)
target_sources(reconverse PRIVATE conv-conds.cpp convcore.cpp random.cpp 
                scheduler.cpp scheduler_registry.cpp scheduler_registered.cpp scheduler_randomized.cpp
                scheduler_old.cpp cpuaffinity.cpp mempool.C collectives.cpp
                comm_backend/comm_backend_internal.cpp threads.cpp cldb.rand.cpp cldb.cpp cmirdmautils.cpp 
                conv-rdma.cpp conv-topology.cpp msgmgr.cpp queueing.cpp cmishmem.cpp
                conv-taskq.cpp persist-comm.cpp
                ccs/conv-ccs.cpp ccs/middle-ccs.cpp ccs/ccs-server.cpp
                ccs/ccs-auth.cpp ccs/sockRoutines.cpp)

# CCS client library: a program outside the parallel job links it to send
# requests to a ++server run. It needs no Converse; ccs-client.cpp includes
# the socket and authentication sources itself.
add_library(ccs-client STATIC ccs/ccs-client.cpp)
target_include_directories(ccs-client PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(
  reconverse PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
                     
add_subdirectory(comm_backend)

# configure needs to be called after add_subdirectory(comm_backend)
configure_file(converse_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/converse_config.h)
