# A crash must leave a stack trace on stderr that names the function the
# program was in. Both runs end with a signal, which ctest reports as an
# exception no matter what PASS_REGULAR_EXPRESSION says, so a shell wrapper
# swallows the status and the output alone decides.
# Windows has no sh; a CMake script does the same job there.
add_reconverse_executable(backtrace backtrace.cpp)
if(WIN32)
  add_test(NAME backtrace-abort
    COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:backtrace> "-DARGS=+pe 1"
            -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
  add_test(NAME backtrace-segv
    COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:backtrace> "-DARGS=+pe 1 segv"
            -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
else()
  add_test(NAME backtrace-abort
    COMMAND sh -c "$<TARGET_FILE:backtrace> +pe 1 2>&1; true")
  add_test(NAME backtrace-segv
    COMMAND sh -c "$<TARGET_FILE:backtrace> +pe 1 segv 2>&1; true")
endif()
set_tests_properties(backtrace-abort PROPERTIES
  PASS_REGULAR_EXPRESSION "Stack trace.*backtrace_test_middle" TIMEOUT 30)
set_tests_properties(backtrace-segv PROPERTIES
  PASS_REGULAR_EXPRESSION "Fatal signal.*SIGSEGV.*backtrace_test_middle" TIMEOUT 30)
