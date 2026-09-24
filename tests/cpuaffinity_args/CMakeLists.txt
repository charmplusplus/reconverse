add_reconverse_executable(cpuaffinity_args cpuaffinity_args.cpp)

# Runs in a single process with no launcher, like tests/args, and must pass
# both with and without hwloc: consuming the flags is the part that does not
# depend on affinity being compiled in. +pemap L0 uses a logical index so the
# map is valid inside whatever cpuset CI hands out.
add_test(NAME cpuaffinity_args
         COMMAND cpuaffinity_args +pe 2 +setcpuaffinity +pemap L0 +showcpuaffinity)
set_tests_properties(cpuaffinity_args PROPERTIES TIMEOUT 60)
