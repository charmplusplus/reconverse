add_reconverse_executable(rdma_ping ping.cpp)
add_test(NAME rdma_ping COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:rdma_ping> +pe 2)
