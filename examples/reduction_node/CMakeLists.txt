add_reconverse_executable(nodereduction reduction.cpp)
add_test(NAME node-reduction-singlenode COMMAND nodereduction +pe 7)
add_test(NAME node-reduction-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 --use-hwthread-cpus $<TARGET_FILE:reduction> +pe 4)
