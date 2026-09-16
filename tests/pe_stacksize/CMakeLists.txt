# A handler with a 1 MB stack frame runs on every PE, including PEs whose
# thread was created by CmiStartThreads. Before the PE threads asked for an
# 8 MB stack this crashed with SIGBUS on macOS, where secondary pthreads get
# only 512 KB.
add_reconverse_executable(pe_stacksize pe_stacksize.cpp)
add_test(NAME pe_stacksize COMMAND pe_stacksize +pe 4)
add_test(NAME pe_stacksize-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:pe_stacksize> +pe 4)
set_tests_properties(pe_stacksize pe_stacksize-multinode PROPERTIES TIMEOUT 60)
