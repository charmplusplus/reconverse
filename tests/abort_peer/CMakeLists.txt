# One PE calls CmiAbort while every PE is exchanging messages. The run must
# end, and end unsuccessfully; survivors that hang after a peer aborts show
# up as the timeout.
# Single process: CmiAbort ends the process with a signal, which ctest
# reports as an exception that WILL_FAIL does not invert, so a shell wrapper
# negates the status. Multi-process: the launcher itself exits with a
# nonzero status (not a signal), so WILL_FAIL applies; the launcher may be a
# list ("mpirun;--oversubscribe"), which is why it is not put inside the
# shell string.
add_reconverse_executable(abort_peer abort_peer.cpp)
add_test(NAME abort_peer COMMAND sh -c "! $<TARGET_FILE:abort_peer> +pe 4")
add_test(NAME abort_peer-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:abort_peer> +pe 4)
set_tests_properties(abort_peer PROPERTIES TIMEOUT 60)
set_tests_properties(abort_peer-multinode PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
