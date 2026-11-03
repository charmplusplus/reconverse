# Every PE of one process puts from one shared buffer at the same time, so
# they race to register it on each target's LCI device.
add_reconverse_executable(rdma_shared_buffer rdma_shared_buffer.cpp)
if(RECONVERSE_ENABLE_COMM_LCI2)
  foreach(ndevices 2 4)
    add_test(NAME rdma_shared_buffer-${ndevices}dev
      COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:rdma_shared_buffer>
              +pe 8 +lci_ndevices ${ndevices})
    set_tests_properties(rdma_shared_buffer-${ndevices}dev PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "every put PASS")
  endforeach()
endif()
