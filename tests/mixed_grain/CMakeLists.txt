# Node-request latency under local load (issue #268); the tests run it
# briefly under each scheduler and mode so it stays buildable. For numbers run
# it by hand, e.g. lcrun -n 2 mixed_grain +pe 8 1000 10000 200.
add_reconverse_executable(mixed_grain mixed_grain.cpp)
add_test(NAME mixed_grain-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
  $<TARGET_FILE:mixed_grain> +pe 4 50 500 50)
add_test(NAME mixed_grain-nodeprio-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
  $<TARGET_FILE:mixed_grain> +pe 4 50 500 50 nodeprio)
add_test(NAME mixed_grain-old-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
  $<TARGET_FILE:mixed_grain> +pe 4 50 500 50 node +old-scheduler)
set_tests_properties(mixed_grain-multinode mixed_grain-nodeprio-multinode
  mixed_grain-old-multinode
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "latency us p50")
