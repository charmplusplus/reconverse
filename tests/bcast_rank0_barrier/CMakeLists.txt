# A broadcast reaches every PE of a process whose rank 0 is blocked in
# CmiNodeBarrier before its scheduler runs (#261).
add_reconverse_executable(bcast_rank0_barrier bcast_rank0_barrier.cpp)
# Over the network: the copy for process 1 comes off the backend.
add_test(NAME bcast_rank0_barrier COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:bcast_rank0_barrier> +pe 4)
# Over the shared-memory pool: the two processes share this host.
add_test(NAME bcast_rank0_barrier-ipc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
         $<TARGET_FILE:bcast_rank0_barrier> +pe 4 +ipc +expect-ipc)
# Six processes: process 1 relays to process 5 while its rank 0 is blocked.
add_test(NAME bcast_rank0_barrier-6proc COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 6
         $<TARGET_FILE:bcast_rank0_barrier> +pe 12)
set_tests_properties(bcast_rank0_barrier bcast_rank0_barrier-ipc
  bcast_rank0_barrier-6proc PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "every PE received the broadcast PASS")
