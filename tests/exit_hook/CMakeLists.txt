# CmiSetExitHook: the hook runs once per process on the main thread, before
# ConverseExit ends the process with its exit code. run_and_capture.cmake
# reports the output and the exit status.
add_reconverse_executable(exit_hook exit_hook.cpp)

add_test(NAME exit_hook
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:exit_hook> "-DARGS=+pe 4"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(exit_hook PROPERTIES
  PASS_REGULAR_EXPRESSION "hook call 1, code 5, main thread.*exit status: 5"
  FAIL_REGULAR_EXPRESSION "hook call [2-9];NOT the main thread"
  TIMEOUT 30)
