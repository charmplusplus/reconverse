add_reconverse_executable(ipc_test ipc_test.cpp)
# One process: nothing to share memory with, so +ipc declines and every
# message still has to arrive.
add_test(NAME ipc-onenode COMMAND ipc_test +pe 2 +ipc)
# The cases that matter: the same traffic with the pool on and with it off.
add_test(NAME ipc-off COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_test> +pe 4)
add_test(NAME ipc-shm COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_test> +pe 4 +ipc +ipcmode shm)
add_test(NAME ipc-auto COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 3
         $<TARGET_FILE:ipc_test> +pe 3 +ipc)
# A pool small enough that the allocator has to recycle blocks, and a cutoff
# well under the largest message, so both paths get exercised in one run.
add_test(NAME ipc-smallpool COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_test> +pe 4 +ipc ++ipcpoolsize 262144)
# Same traffic under the original scheduler loop: the pool is drained from
# both scheduler implementations, and this is the one that covers the other.
add_test(NAME ipc-shm-old-scheduler COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_test> +pe 4 +ipc +ipcmode shm +old-scheduler)
