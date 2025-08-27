add_reconverse_executable(random_test random_test.cpp)
add_test(NAME random_test COMMAND random_test +p 2)
