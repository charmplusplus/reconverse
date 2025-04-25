add_reconverse_executable(kNeighbors kNeighbors.cpp)
add_test(NAME kNeighbors COMMAND kNeighbors +pe 4 2 100 100)