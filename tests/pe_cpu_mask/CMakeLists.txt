# Every PE thread starts with the process's CPU mask, even when opening the
# network binds the main thread to one CPU (psm2 on Omni-Path does; see
# reconverse#320). The one-process run is the case that hung: the backend
# opens and closes the network, and the PE threads are created afterwards.
add_reconverse_executable(pe_cpu_mask pe_cpu_mask.cpp)
add_test(NAME pe_cpu_mask COMMAND pe_cpu_mask +pe 4)
add_test(NAME pe_cpu_mask-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:pe_cpu_mask> +pe 4)
set_tests_properties(pe_cpu_mask pe_cpu_mask-multinode PROPERTIES TIMEOUT 60)
