add_reconverse_executable(reduction reduction.cpp)
add_test(NAME reduction COMMAND reduction +pe 7)
add_test(NAME reduction-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:reduction> +pe 4)
