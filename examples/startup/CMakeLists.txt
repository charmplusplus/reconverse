add_reconverse_executable(startup startup.cpp)
add_test(NAME startup COMMAND startup +p 4)
