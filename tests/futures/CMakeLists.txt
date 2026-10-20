# A thread on PE 0 waits on one Cfuture per PE while the other PEs fill them
# remotely; checks the values and the zero-copy buffer form.
add_reconverse_executable(futures futures.cpp)
add_test(NAME futures COMMAND $<TARGET_FILE:futures> +pe 4)
add_test(NAME futures-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:futures> +pe 4)
set_tests_properties(futures futures-multinode PROPERTIES TIMEOUT 60)
