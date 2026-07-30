add_reconverse_executable(sched_rate sched_rate.cpp)
add_test(NAME ult_sched_rate COMMAND sched_rate +pe 4 -threads 16 -yields 100 -reps 2)

add_reconverse_executable(ctxswitch ctxswitch.cpp)
add_test(NAME ult_ctxswitch COMMAND ctxswitch +pe 4 -max_flows 16 -switches 10000 -reps 2)

# the only target here that needs C++20; the rest of the tree is C++11
add_reconverse_executable(am_vs_coro am_vs_coro.cpp)
set_target_properties(am_vs_coro PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME ult_am_vs_coro COMMAND am_vs_coro +pe 4 -flows 8 -events 20000 -reps 2)
