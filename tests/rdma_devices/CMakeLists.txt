# Zero-copy get and put between two processes, with every combination of the
# buffer owner's and the initiator's LCI device (#203).
add_reconverse_executable(rdma_devices rdma_devices.cpp)
if(RECONVERSE_ENABLE_COMM_LCI2)
  foreach(ndevices 1 2 4)
    add_test(NAME rdma_devices-${ndevices}dev
      COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:rdma_devices>
              +pe 8 +lci_ndevices ${ndevices})
    set_tests_properties(rdma_devices-${ndevices}dev PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "every get and put PASS")
  endforeach()
endif()
