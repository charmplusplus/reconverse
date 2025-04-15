add_reconverse_executable(wnbcast wnbcast.C)
add_test(NAME wnbcast-onenode COMMAND wnbcast +pe 4)
add_test(NAME wnbcast COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 wnbcast +pe 4)
