if(RECONVERSE_ENABLE_CUDA OR RECONVERSE_ENABLE_HIP)
  add_reconverse_executable(rdma_pingpong_device pingpong_device.cpp)
  add_test(NAME rdma_pingpong_device
           COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
                   $<TARGET_FILE:rdma_pingpong_device> +pe 2)
endif()
