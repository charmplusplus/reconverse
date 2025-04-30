add_reconverse_executable(queue queue.cpp)
add_reconverse_executable(barrier barrier.cpp)
# The test `queue` needs to access internal headers
target_include_directories(queue PRIVATE ../src/)
set_target_properties(queue PROPERTIES CXX_STANDARD 17)
set_target_properties(barrier PROPERTIES CXX_STANDARD 17)
