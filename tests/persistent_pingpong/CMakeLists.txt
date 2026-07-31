add_reconverse_executable(persistent_pingpong pingpong.cpp)
add_test(NAME persistent_pingpong COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:persistent_pingpong> +pe 1)
