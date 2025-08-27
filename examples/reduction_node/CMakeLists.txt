add_reconverse_executable(nodereduction reduction.cpp)
add_test(NAME node-reduction-singlenode COMMAND nodereduction +p 7)
add_test(NAME node-reduction-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:reduction> +p 8)
