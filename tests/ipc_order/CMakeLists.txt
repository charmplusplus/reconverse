add_reconverse_executable(ipc_order ipc_order.cpp)
# Two processes on one host, one PE each: one producer and one consumer for
# the pool's receive queue, which is what makes the delivery order well
# defined (see the note in ipc_order.cpp).
add_test(NAME ipc_order COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 2 +ipc +expect-ipc)
# The same burst over the backend. The backend promises no ordering either,
# so this one only has to deliver every message intact.
add_test(NAME ipc_order-noipc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 2)
# The original scheduler loop drains the pool through a different path.
add_test(NAME ipc_order-old-scheduler COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:ipc_order> +pe 2 +ipc +expect-ipc +old-scheduler)
