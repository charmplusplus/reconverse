add_reconverse_executable(finalize finalize.cpp)
add_test(NAME finalize COMMAND finalize +pe 4)
set_tests_properties(finalize PROPERTIES PASS_REGULAR_EXPRESSION "finalized cleanly, main returning")
