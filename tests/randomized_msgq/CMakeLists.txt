add_reconverse_executable(randomized_msgq randomized_msgq.cpp)
# The same program checks FIFO order under the default scheduler and a
# shuffled order under +randomized_msgq; its first argument says which.
add_test(NAME randomized_msgq-fifo COMMAND randomized_msgq fifo +pe 2)
add_test(NAME randomized_msgq-random COMMAND randomized_msgq random +pe 2 +randomized_msgq)
add_test(NAME randomized_msgq-random-seeded COMMAND randomized_msgq random +pe 2 +randomized_msgq +randomized_seed 12345)
# Same seed, same draws: the per-PE run-order digests of two seeded runs
# must match (repeat.cmake runs the program twice and compares them).
add_test(NAME randomized_msgq-random-seeded-repeat
  COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:randomized_msgq>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/repeat.cmake)
