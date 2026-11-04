add_reconverse_executable(ipc_order ipc_order.cpp)
# Two processes on one host, one PE each: one burst from PE 0 to PE 1, which
# must arrive in the order sent (see the note in ipc_order.cpp).
add_test(NAME ipc_order COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 2 +ipc +expect-ipc)
# Four PEs per process: each has its own receive queue, and four bursts,
# rank r to rank r, cross the pool at once; each must still arrive in order.
add_test(NAME ipc_order-multi COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 8 +ipc +expect-ipc)
# The same burst over the backend. The backend promises no ordering either,
# so this one only has to deliver every message intact.
add_test(NAME ipc_order-noipc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 2)
# The original scheduler loop drains the pool through a different path.
add_test(NAME ipc_order-old-scheduler COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 2 +ipc +expect-ipc +old-scheduler)
