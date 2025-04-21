add_reconverse_executable(random_test random_test.C)
add_test(NAME random_test COMMAND random_test +pe 2)