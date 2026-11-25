add_reconverse_executable(urgent urgent.cpp)
add_test(NAME urgent COMMAND urgent +pe 2)
add_test(NAME urgent_2proc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:urgent> +pe 2)
