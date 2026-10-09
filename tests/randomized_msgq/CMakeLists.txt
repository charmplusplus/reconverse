add_reconverse_executable(randomized_msgq randomized_msgq.cpp)
# The same program checks FIFO order under the default scheduler and a
# shuffled order under +randomized_msgq; its first argument says which.
add_test(NAME randomized_msgq-fifo COMMAND randomized_msgq fifo +pe 2)
add_test(NAME randomized_msgq-random COMMAND randomized_msgq random +pe 2 +randomized_msgq)
add_test(NAME randomized_msgq-random-seeded COMMAND randomized_msgq random +pe 2 +randomized_msgq +randomized_seed 12345)
