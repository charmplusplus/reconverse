add_reconverse_executable(fanout_refcount fanout_refcount.cpp)
add_test(NAME fanout_refcount-onepe COMMAND fanout_refcount +pe 1)
add_test(NAME fanout_refcount COMMAND fanout_refcount +pe 4)
add_test(NAME fanout_refcount-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 3 $<TARGET_FILE:fanout_refcount> +pe 6)
add_test(NAME fanout_refcount-multinode4 COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:fanout_refcount> +pe 8)

# Timing harness, not a ctest test: it compares the old per-destination-PE
# fan-out with the per-destination-process one in a single binary.
add_reconverse_executable(fanout_bench fanout_bench.cpp)
