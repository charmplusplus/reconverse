add_reconverse_executable(send_variants send_variants.cpp)
add_test(NAME send_variants COMMAND send_variants +pe 4)
add_test(NAME send_variants-onepe COMMAND send_variants +pe 1)
add_test(NAME send_variants-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:send_variants> +pe 4)
