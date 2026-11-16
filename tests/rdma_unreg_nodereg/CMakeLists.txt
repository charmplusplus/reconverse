# Gets and puts between two processes with CMK_BUFFER_UNREG and
# CMK_BUFFER_NODEREG on both sides leave no registration behind (#331).
add_reconverse_executable(rdma_unreg_nodereg rdma_unreg_nodereg.cpp)
if(RECONVERSE_ENABLE_COMM_LCI2)
  foreach(ndevices 1 2)
    add_test(NAME rdma_unreg_nodereg-${ndevices}dev
      COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:rdma_unreg_nodereg>
              +pe 4 +lci_ndevices ${ndevices})
    set_tests_properties(rdma_unreg_nodereg-${ndevices}dev PROPERTIES
      TIMEOUT 60 PASS_REGULAR_EXPRESSION "no registration left PASS")
  endforeach()
endif()
