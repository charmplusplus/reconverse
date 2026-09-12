add_reconverse_executable(cld_test cld_test.cpp)
add_test(NAME cld COMMAND cld_test +pe 4)
add_test(NAME cld-onepe COMMAND cld_test +pe 1)
add_test(NAME cld-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:cld_test> +pe 4)
