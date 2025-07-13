add_subdirectory(broadcast)
add_subdirectory(group)
add_subdirectory(within-node-bcast)
add_subdirectory(conds)
add_subdirectory(idle)
add_subdirectory(kNeighbors)
add_subdirectory(orig-converse/pingpong)
add_subdirectory(ping_ack)
add_subdirectory(random)
add_subdirectory(ring)
add_subdirectory(startup)
add_subdirectory(reduction)
add_subdirectory(rdma_pingpong)
add_subdirectory(reduction_dual)
add_subdirectory(reduction_node)
add_subdirectory(cth)
add_subdirectory(self_send)
add_subdirectory(ctv)

# Add a test that configures, builds, and runs standalone_project
add_test(NAME standalone_project_build_and_test
  COMMAND ${CMAKE_COMMAND}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_standalone_test.cmake
)
