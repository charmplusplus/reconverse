# Leave the scheduler with many large sends still in flight (see
# exit_inflight.cpp). Needs two processes: the buffers of sends to another
# process are freed by the LCI completion, and the bug was that a PE's mempool
# could be destroyed in ConverseExit before those completions ran.
add_reconverse_executable(exit_inflight exit_inflight.cpp)
add_test(NAME exit_inflight-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:exit_inflight> 400 65536 +pe 4)
set_tests_properties(exit_inflight-multinode PROPERTIES TIMEOUT 120)
