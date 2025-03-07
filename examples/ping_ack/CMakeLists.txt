# We have to explicitly tell cmake this for files with .c suffix
set_source_files_properties(ping.c PROPERTIES LANGUAGE CXX)
add_reconverse_executable(ping_ack ping.c)
add_test(NAME ping_ack COMMAND ping_ack +pe 4)