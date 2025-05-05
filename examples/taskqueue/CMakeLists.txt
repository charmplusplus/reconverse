add_reconverse_executable(taskqueue taskqueue.cpp)
add_test(NAME task_queue COMMAND task_queue +pe 7)