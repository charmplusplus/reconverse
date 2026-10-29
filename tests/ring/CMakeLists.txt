add_reconverse_executable(ring ring.cpp)
add_test(NAME ring COMMAND ring +pe 4)

# Each process runs 4 ranks on 2 LCI devices (ranks 0-1 on device 0, 2-3 on
# device 1), and +backend_poll_thread 4 leaves rank 0 as the only rank that
# polls by that rule. Device 1 must still get a poller (its first thread,
# rank 2): rank 3 sends the exit message on it, and without one the other
# process never receives it and the run hangs (#306).
if(RECONVERSE_ENABLE_COMM_LCI2)
  add_test(NAME ring-backend-poll-thread-multinode
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:ring>
            +pe 8 +lci_ndevices 2 +backend_poll_thread 4)
  set_tests_properties(ring-backend-poll-thread-multinode PROPERTIES TIMEOUT 60)
endif()
