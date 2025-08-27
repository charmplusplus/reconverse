add_reconverse_executable(kNeighbors kNeighbors.cpp)
add_test(NAME kNeighbors COMMAND kNeighbors +p 4 2 100 100)
