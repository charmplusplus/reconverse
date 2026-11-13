# CmiSetAbortHook: the hook runs once per process, before CmiAbort aborts,
# and the other PEs wait for it. The single-process runs end in an abort or
# an _exit, so run_and_capture.cmake reports the output and the exit status
# and the expressions decide. A second "hook entered" line means a second
# thread ran the hook.
add_reconverse_executable(abort_hook abort_hook.cpp)

# The hook returns: CmiAbort must still end the process.
add_test(NAME abort_hook
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_hook> "-DARGS=+pe 4"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_hook PROPERTIES
  PASS_REGULAR_EXPRESSION "hook returns: calls 1, code 1, reason \\[abort_hook: PE [0-9]+ aborts\\], 4 of 4 PEs aborting"
  FAIL_REGULAR_EXPRESSION "hook entered, call [2-9];exit status: 0"
  TIMEOUT 30)

# The hook ends the process itself, as MPI_Abort would: its status wins.
add_test(NAME abort_hook-exit
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_hook> "-DARGS=+pe 4 exit"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_hook-exit PROPERTIES
  PASS_REGULAR_EXPRESSION "exit status: 3"
  FAIL_REGULAR_EXPRESSION "hook entered, call [2-9]"
  TIMEOUT 30)

# The hook never returns: the waiting PEs give up after 10 s and abort, so the
# process ends anyway (a timeout here means they waited forever).
add_test(NAME abort_hook-block
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_hook> "-DARGS=+pe 4 block"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_hook-block PROPERTIES
  PASS_REGULAR_EXPRESSION "hook blocks: 4 of 4 PEs aborting"
  FAIL_REGULAR_EXPRESSION "hook entered, call [2-9];exit status: 0"
  TIMEOUT 45)

# Two processes of two PEs, each process's hook ending it with status 3. The
# launcher may kill one process before its hook has finished, so the test
# requires one completed hook, not two.
add_test(NAME abort_hook-multinode
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:abort_hook> +pe 4 exit)
set_tests_properties(abort_hook-multinode PROPERTIES
  PASS_REGULAR_EXPRESSION "hook ends the process: calls 1, code 1, reason \\[abort_hook: PE [0-9]+ aborts\\], 2 of 2 PEs aborting"
  FAIL_REGULAR_EXPRESSION "hook entered, call [2-9]"
  TIMEOUT 60)
