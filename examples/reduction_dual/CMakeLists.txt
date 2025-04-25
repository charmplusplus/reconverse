add_reconverse_executable(reduction_dual reduction_dual.C)
add_test(NAME reduction_dual COMMAND reduction_dual +pe 7)
add_test(NAME reduction_dual-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:reduction_dual> +pe 4)
