add_reconverse_executable(startup startup.cpp)
add_test(NAME startup COMMAND startup +pe 4)
