# We have to explicitly tell cmake this for files with .c suffix
# set_source_files_properties(ping.c PROPERTIES LANGUAGE CXX)
add_reconverse_executable(ping_ack ping.C)
add_test(NAME ping_ack COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:ping_ack> +pe 4)
