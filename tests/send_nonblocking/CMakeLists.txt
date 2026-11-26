# A PE sends many messages to a process that is not receiving, and must get
# back from the sends and run its next handler meanwhile (see
# send_nonblocking.cpp). Needs two processes.
add_reconverse_executable(send_nonblocking send_nonblocking.cpp)
add_test(NAME send_nonblocking-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:send_nonblocking> +pe 2)
set_tests_properties(send_nonblocking-multinode PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "send_nonblocking: PASS")
