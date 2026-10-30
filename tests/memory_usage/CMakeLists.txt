add_reconverse_executable(memory_usage memory_usage.cpp)
add_test(NAME memory_usage COMMAND memory_usage +pe 2)
set_tests_properties(memory_usage
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "memory_usage: PASS")
