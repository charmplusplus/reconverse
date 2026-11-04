add_reconverse_executable(ipc_poolfull ipc_poolfull.cpp)
# A 64 KiB segment and a burst of 400 1 KiB messages that the receiver holds
# until all have arrived: the segment fills, the rest go over the network,
# and the freed blocks come back to their sender for the second burst.
add_test(NAME ipc_poolfull COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_poolfull> +pe 2 +ipc ++ipcpoolsize 65536)
add_test(NAME ipc_poolfull-multi COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_poolfull> +pe 4 +ipc ++ipcpoolsize 65536)
