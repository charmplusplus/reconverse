add_reconverse_executable(conds conds.C)
add_test(NAME conds COMMAND conds +pe 4)