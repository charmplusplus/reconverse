# Bursts of mixed-size sends between the PEs of two processes, to PEs that
# are busy from time to time; every message is accounted for (see
# send_stress.cpp). Several PEs per process share the backend's send queues.
add_reconverse_executable(send_stress send_stress.cpp)
add_test(NAME send_stress-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:send_stress> 100 16 +pe 4)
set_tests_properties(send_stress-multinode PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "send_stress: .*PASS")
