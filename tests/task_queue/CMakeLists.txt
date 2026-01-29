add_reconverse_executable(task_queue task_queue.cpp)
add_test(NAME task_queue COMMAND task_queue +pe 4)