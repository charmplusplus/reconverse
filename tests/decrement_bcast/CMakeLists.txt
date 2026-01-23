add_reconverse_executable(decrement_bcast decrement_bcast.cpp)
# run with 4 PEs by default to exercise multiple PEs; override in ctest if desired
add_test(NAME decrement_bcast COMMAND decrement_bcast +pe 4)
