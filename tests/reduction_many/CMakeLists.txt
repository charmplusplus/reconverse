# Hundreds of reductions per PE (and node reductions per process), 8 in
# flight at a time: ids keep counting past the 16-slot reduction table.
add_reconverse_executable(reduction_many reduction_many.cpp)
add_test(NAME reduction_many COMMAND reduction_many +pe 7)
add_test(NAME reduction_many-node COMMAND reduction_many +pe 7 node)
add_test(NAME reduction_many-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:reduction_many> +pe 4)
add_test(NAME reduction_many-node-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:reduction_many> +pe 8 node)
set_tests_properties(reduction_many reduction_many-node
  reduction_many-multinode reduction_many-node-multinode
  PROPERTIES PASS_REGULAR_EXPRESSION "all correct")
# 17 in flight is one more than the table holds: PE 0 starts all 17 before
# its children can finish the first, so it must abort with a clear message.
# CmiAbort ends the process with a signal, which ctest reports as a failure
# whatever the output, so a shell wrapper turns it into an exit status (as
# in abort_peer).
add_test(NAME reduction_many-overflow
  COMMAND sh -c "! $<TARGET_FILE:reduction_many> +pe 7 pe 1 17")
set_tests_properties(reduction_many-overflow
  PROPERTIES PASS_REGULAR_EXPRESSION "Too many simultaneous reductions")
