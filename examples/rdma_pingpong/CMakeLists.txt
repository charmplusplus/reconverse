# We have to explicitly tell cmake this for files with .c suffix
# set_source_files_properties(ping.c PROPERTIES LANGUAGE CXX)
add_reconverse_executable(rdma_pingpong pingpong.cpp)
add_test(NAME rdma_pingpong COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:rdma_pingpong> +pe 2)
