# CmiAbort must print a 600-character reason in full. The run ends with
# SIGABRT, so a shell wrapper swallows the status and the output decides.
# Windows has no sh; a CMake script does the same job there.
add_reconverse_executable(abort_message abort_message.cpp)
if(WIN32)
  add_test(NAME abort_message
    COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:abort_message> "-DARGS=+pe 1"
            -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
else()
  add_test(NAME abort_message
    COMMAND sh -c "$<TARGET_FILE:abort_message> +pe 1 2>&1; true")
endif()
set_tests_properties(abort_message PROPERTIES
  PASS_REGULAR_EXPRESSION "chunk-09-0+-ends-here" TIMEOUT 30)
