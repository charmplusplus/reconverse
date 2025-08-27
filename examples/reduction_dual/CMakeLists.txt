add_reconverse_executable(reduction_dual reduction_dual.cpp)
add_test(NAME reduction_dual COMMAND reduction_dual +p 7)
add_test(NAME reduction_dual-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:reduction_dual> +p 4)
