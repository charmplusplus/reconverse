# CmiRegisterAbortFlush: the registered functions run once per process, in
# order, before the abort hook, while the other aborting PEs wait. The runs
# end in an abort, so run_and_capture.cmake reports the output and the exit
# status and the expressions decide.
add_reconverse_executable(abort_flush abort_flush.cpp)

add_test(NAME abort_flush
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_flush> "-DARGS=+pe 4"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_flush PROPERTIES
  PASS_REGULAR_EXPRESSION "B done, call 1, position 2, A calls 1.*hook: A calls 1, B calls 1, 4 of 4 PEs aborting"
  FAIL_REGULAR_EXPRESSION "A entered, call [2-9];B done, call [2-9];exit status: 0"
  TIMEOUT 30)

# A CmiAbort inside a flush function skips the rest of the step and still
# reaches the hook.
add_test(NAME abort_flush-nested
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_flush> "-DARGS=+pe 4 nested"
          -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
set_tests_properties(abort_flush-nested PROPERTIES
  PASS_REGULAR_EXPRESSION "CmiAbort inside A.*hook: A calls 1, B calls 0"
  FAIL_REGULAR_EXPRESSION "A entered, call [2-9];B done;exit status: 0"
  TIMEOUT 30)
