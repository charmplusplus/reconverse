add_reconverse_executable(reduction reduction.cpp)
add_test(NAME reduction COMMAND reduction +pe 7)
add_test(NAME reduction-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:reduction> +pe 4)
# Same run with the shared-memory IPC pool carrying the traffic between
# the two processes, which is a different delivery path for every send
# above: a pool block rather than an active message.
add_test(NAME reduction-multinode-ipc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:reduction> +pe 4 +ipc)
