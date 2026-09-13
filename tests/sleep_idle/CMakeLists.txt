add_reconverse_executable(sleep_idle sleep_idle.cpp)
add_test(NAME sleep_idle_on COMMAND sleep_idle +pe 4 +CmiSleepOnIdle)
add_test(NAME sleep_idle_off COMMAND sleep_idle +pe 4)
