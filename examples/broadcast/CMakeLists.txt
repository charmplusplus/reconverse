add_reconverse_executable(broadcast broadcast.cpp)
add_test(NAME broadcast-onenode COMMAND broadcast +p 4)
add_test(NAME broadcast COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:broadcast> +p 4)
