target_include_directories(reconverse PRIVATE .)
target_sources(reconverse PRIVATE conv-conds.cpp convcore.cpp random.cpp 
                scheduler.cpp cpuaffinity.cpp collectives.cpp
                comm_backend/comm_backend_internal.cpp threads.cpp cldb.rand.cpp cldb.cpp cmirdmautils.cpp 
                conv-rdma.cpp conv-topology.cpp msgmgr.cpp
                pup/pup_xlater.cpp pup/pup_toNetwork.cpp pup/pup_c.cpp 
                pup/pup_paged.cpp pup/pup_toNetwork4.cpp pup/pup_util.cpp 
                pup/pup_cmialloc.cpp
                conv-ccs/conv-ccs.cpp conv-ccs/ccs-server.cpp conv-ccs/ccs-client.cpp 
                conv-ccs/ccs-auth.cpp conv-ccs/ccs-builtins.cpp conv-ccs/middle-ccs.cpp)
target_include_directories(
  reconverse PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Add pup directory to include path                     
target_include_directories(reconverse PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pup>
  $<INSTALL_INTERFACE:include/pup>)
                     
add_subdirectory(comm_backend)

# Set pointer size flag for PUP
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(CMK_SIZET_64BIT ON)
else()
  set(CMK_SIZET_64BIT OFF)
endif()

# configure needs to be called after add_subdirectory(comm_backend)
configure_file(converse_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/converse_config.h)
