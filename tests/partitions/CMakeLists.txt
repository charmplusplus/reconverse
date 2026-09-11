add_reconverse_executable(partitions partitions.cpp)

add_test(NAME partitions-single COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:partitions> +pe 4 -expect-partitions 1)
add_test(NAME partitions-two COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:partitions> +pe 8 +partitions 2 -expect-partitions 2)
add_test(NAME partitions-four COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:partitions> +pe 4 +partitions 4 -expect-partitions 4)
add_test(NAME partitions-replicas COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:partitions> +pe 8 +replicas 2 -expect-partitions 2)

# A misrouted send strands the PEs waiting on it rather than tripping an
# assertion, so cap the wait instead of leaving it to the default timeout.
set_tests_properties(partitions-single partitions-two partitions-four partitions-replicas
                     PROPERTIES TIMEOUT 120)
