# Per-sender order and loss through the PE thread queue, across ring growth,
# pushes to the PE's own queue and steady streaming (issue #271), with the
# shared FIFO (default), with +thread_queue_per_producer, and under the old
# scheduler. +randomized_msgq reorders on purpose, so it is not run here.
add_reconverse_executable(thread_queue_order thread_queue_order.cpp)
add_test(NAME thread_queue_order COMMAND thread_queue_order +pe 8)
add_test(NAME thread_queue_order-per_producer
  COMMAND thread_queue_order +pe 8 +thread_queue_per_producer)
add_test(NAME thread_queue_order-old
  COMMAND thread_queue_order +pe 8 +old-scheduler)
set_tests_properties(thread_queue_order thread_queue_order-per_producer
  thread_queue_order-old PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "thread_queue_order: PASS")
