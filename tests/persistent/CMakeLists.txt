add_reconverse_executable(persistent persistent.cpp)

# Two processes with two PEs each: the ring channels cross nodes as well as
# staying within one, so both the one-sided and the intra-node paths run.
add_test(NAME persistent COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:persistent> +pe 4)

# Same, with one-sided transfers disabled, to cover the message-based fallback.
add_test(NAME persistent_nordma COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:persistent> +pe 4 +nordma)

# A single process: every channel is intra-node, so this covers channel setup,
# teardown, and the intra-node send path rather than the receive buffers.
add_test(NAME persistent_single_node COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 1 $<TARGET_FILE:persistent> +pe 2)
