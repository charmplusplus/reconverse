add_reconverse_executable(startup startup.C)
add_test(NAME startup COMMAND startup +pe 4)