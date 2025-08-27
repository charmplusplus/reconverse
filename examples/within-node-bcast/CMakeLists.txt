add_reconverse_executable(within-node-bcast wnbcast.cpp)
add_test(NAME within-node-bcast-onenode COMMAND within-node-bcast +pe 4)
add_test(NAME within-node-bcast COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:within-node-bcast> +pe 4)
