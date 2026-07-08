add_reconverse_executable(cross_node_enqueue cross_node_enqueue.cpp)
add_test(NAME cross_node_enqueue COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:cross_node_enqueue> +pe 2 -num_msgs 20)
