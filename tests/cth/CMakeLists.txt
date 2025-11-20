add_reconverse_executable(pgm pgm.cpp)
add_test(NAME cth COMMAND pgm +pe 4)
