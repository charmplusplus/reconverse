add_reconverse_executable(within-node-bcast wnbcast.cpp)
add_test(NAME within-node-bcast-onenode COMMAND within-node-bcast +pe 4)
add_test(NAME within-node-bcast COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:within-node-bcast> +pe 4)
# Same run with the shared-memory IPC pool carrying the traffic between
# the two processes, which is a different delivery path for every send
# above: a pool block rather than an active message.
add_test(NAME within-node-bcast-ipc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:within-node-bcast> +pe 4 +ipc)
