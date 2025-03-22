add_reconverse_executable(queue queue.C)
# The test `queue` needs to access internal headers
target_include_directories(queue PRIVATE ../src/)