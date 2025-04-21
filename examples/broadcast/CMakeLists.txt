add_reconverse_executable(broadcast broadcast.C)
add_test(NAME broadcast-onenode COMMAND broadcast +pe 4)
add_test(NAME broadcast COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:broadcast> +pe 4)
