add_reconverse_executable(queue_test queue_test.cpp)
add_test(NAME queue COMMAND queue_test +pe 2)
