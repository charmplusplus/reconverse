# CmiAbort must print a 600-character reason in full. The run ends with
# SIGABRT, so a shell wrapper swallows the status and the output decides.
add_reconverse_executable(abort_message abort_message.cpp)
add_test(NAME abort_message
  COMMAND sh -c "$<TARGET_FILE:abort_message> +pe 1 2>&1; true")
set_tests_properties(abort_message PROPERTIES
  PASS_REGULAR_EXPRESSION "chunk-09-0+-ends-here" TIMEOUT 30)
