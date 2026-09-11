add_reconverse_executable(physical_nodes physical_nodes.cpp)
add_test(NAME physical_nodes COMMAND physical_nodes +pe 2)
add_test(NAME physical_nodes-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:physical_nodes> +pe 4)
