add_reconverse_executable(handlers handlers.cpp)
add_test(NAME handlers COMMAND handlers +pe 4)
add_test(NAME handlers-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:handlers> +pe 4)
