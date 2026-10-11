# A PE that keeps sending to itself must not starve messages from other PEs
# (one process) or from the network (two processes), under either scheduler.
# A starved stop message shows up as the timeout.
add_reconverse_executable(self_loop self_loop.cpp)
add_test(NAME self_loop COMMAND self_loop +pe 2)
add_test(NAME self_loop-old COMMAND self_loop +pe 2 +old-scheduler)
add_test(NAME self_loop-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:self_loop> +pe 2)
add_test(NAME self_loop-multinode-old COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:self_loop> +pe 2 +old-scheduler)
set_tests_properties(self_loop self_loop-old self_loop-multinode self_loop-multinode-old
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "self_loop: PASS")
