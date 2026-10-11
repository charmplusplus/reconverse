add_reconverse_executable(rdma_ipc_ack ack_order.cpp)

# The case the test exists for: two processes on one host with the pool
# carrying their messages, so a destination ack the runtime did not hold back
# would overtake the put it acknowledges.
add_test(NAME rdma_ipc_ack COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:rdma_ipc_ack> +pe 2 +ipc +expect-ipc)

# The same traffic with the pool off, which is what the ordering scope falls
# back to. A failure here is a broken put, not a broken ack.
add_test(NAME rdma_ipc_ack-noipc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:rdma_ipc_ack> +pe 2)

# No one-sided put at all: the ack comes from the destination, after the data,
# and may travel through the pool. Covers the copy-based fallback's ordering.
add_test(NAME rdma_ipc_ack-nordma COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:rdma_ipc_ack> +pe 2 +ipc +expect-ipc +nordma)
