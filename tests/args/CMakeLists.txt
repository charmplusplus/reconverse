add_reconverse_executable(args_test args_test.cpp)
add_test(NAME args COMMAND args_test +pe 2 -int 42 -intglued=7 -long 5000000000
  -dbl 2.5 -str hello -flag -size 3M -old 9)
