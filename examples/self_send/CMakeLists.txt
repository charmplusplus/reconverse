add_reconverse_executable(self_send self_send.cpp)
add_test(NAME self_send COMMAND self_send +pe 1 100 100)
