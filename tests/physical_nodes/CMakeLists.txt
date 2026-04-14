add_reconverse_executable(physical_nodes physical_nodes.cpp)
add_test(NAME physical_nodes COMMAND physical_nodes +pe 2)
