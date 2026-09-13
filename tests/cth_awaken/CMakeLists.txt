add_reconverse_executable(cth_awaken cth_awaken.cpp)
add_test(NAME cth_awaken COMMAND cth_awaken +pe 4)
add_reconverse_executable(cth_primary cth_primary.cpp)
add_test(NAME cth_primary COMMAND cth_primary +pe 4)
