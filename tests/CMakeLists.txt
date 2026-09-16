add_subdirectory(broadcast)
add_subdirectory(group)
add_subdirectory(within-node-bcast)
add_subdirectory(conds)
add_subdirectory(decrement_enqueue)
add_subdirectory(decrement_bcast)
add_subdirectory(idle)
add_subdirectory(kNeighbors)
add_subdirectory(orig-converse/pingpong)
add_subdirectory(ping_ack)
add_subdirectory(random)
add_subdirectory(ring)
add_subdirectory(startup)
add_subdirectory(partitions)
add_subdirectory(persistent)
add_subdirectory(physical_nodes)
add_subdirectory(reduction)
add_subdirectory(rdma_ping)
add_subdirectory(runtime_modes)
add_subdirectory(rdma_pingpong)
add_subdirectory(rdma_pingpong_device)
add_subdirectory(reduction_dual)
add_subdirectory(reduction_node)
add_subdirectory(task_queue)
add_subdirectory(cth)
add_subdirectory(self_send)
add_subdirectory(ctv)
add_subdirectory(ult_bench)
add_subdirectory(queue)
add_subdirectory(cld)
add_subdirectory(args)
add_subdirectory(send_variants)
add_subdirectory(cth_listener)
add_subdirectory(handlers)
add_subdirectory(abort_peer)
add_subdirectory(megarecon)

# Diagnostic programs for issue #219 (fan-out message copies). They are build
# targets only, not ctest tests: fanout_probe deliberately exercises the unsafe
# shape (one buffer shared by sends to several destination PEs) to show that it
# mis-delivers, and fanout_bench is a timing harness.
add_subdirectory(fanout_probe)

# Add a test that configures, builds, and runs standalone_project
add_test(NAME standalone_project_build_and_test
  COMMAND ${CMAKE_COMMAND}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_standalone_test.cmake
)
