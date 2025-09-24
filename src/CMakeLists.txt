target_include_directories(reconverse PRIVATE .)
target_sources(reconverse PRIVATE conv-conds.cpp convcore.cpp random.cpp 
                scheduler.cpp cpuaffinity.cpp collectives.cpp
                comm_backend/comm_backend_internal.cpp threads.cpp cldb.rand.cpp cldb.cpp cmirdmautils.cpp 
                conv-rdma.cpp conv-topology.cpp msgmgr.cpp)
target_include_directories(
  reconverse PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
                     
add_subdirectory(comm_backend)

# configure needs to be called after add_subdirectory(comm_backend)
configure_file(converse_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/converse_config.h)
