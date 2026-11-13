# CmiSetAbortHook (reconverse #321): the hook runs once per process, before
# CmiAbort aborts, and the other PEs wait for it. Both single-process runs end
# in an abort or an _exit, so run_and_capture.cmake reports the output and the
# exit status and the expressions decide. A second "hook entered" line means a
# second thread ran the hook.
add_reconverse_executable(abort_hook abort_hook.cpp)
add_test(NAME abort_hook
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_hook> "-DARGS=+pe 4"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_hook PROPERTIES
  PASS_REGULAR_EXPRESSION "hook returns: calls 1, code 1, reason \\[abort_hook: PE [0-9]+ aborts\\]"
  FAIL_REGULAR_EXPRESSION "hook entered, call [2-9]"
  TIMEOUT 30)
# The hook ends the process itself, as MPI_Abort would: its status wins.
add_test(NAME abort_hook-exit
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_hook> "-DARGS=+pe 4 exit"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_hook-exit PROPERTIES
  PASS_REGULAR_EXPRESSION "exit status: 3"
  FAIL_REGULAR_EXPRESSION "hook entered, call [2-9]"
  TIMEOUT 30)
# Two processes: each runs its own hook, and the run ends unsuccessfully.
add_test(NAME abort_hook-multinode
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:abort_hook> +pe 2 exit)
set_tests_properties(abort_hook-multinode PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
