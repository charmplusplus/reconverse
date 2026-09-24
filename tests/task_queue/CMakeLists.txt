add_reconverse_executable(task_queue task_queue.cpp)
add_test(NAME task_queue COMMAND task_queue +pe 4)
add_test(NAME task_queue-old-scheduler COMMAND task_queue +pe 4 +old-scheduler)
set_tests_properties(task_queue task_queue-old-scheduler PROPERTIES TIMEOUT 60)
