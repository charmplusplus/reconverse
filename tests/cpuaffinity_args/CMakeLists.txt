add_reconverse_executable(cpuaffinity_args cpuaffinity_args.cpp)

# Runs in a single process with no launcher, like tests/args. +pemap L0 uses a
# logical index so the map is valid inside whatever cpuset CI hands out.
# With hwloc the flags bind and are consumed. Without it, a run that asks for
# binding aborts at startup; +showcpuaffinity alone is consumed and reported.
add_test(NAME cpuaffinity_args_show
         COMMAND cpuaffinity_args +pe 2 +showcpuaffinity)
set_tests_properties(cpuaffinity_args_show PROPERTIES TIMEOUT 60)
if(RECONVERSE_ENABLE_CPU_AFFINITY)
  add_test(NAME cpuaffinity_args
           COMMAND cpuaffinity_args +pe 2 +setcpuaffinity +pemap L0 +showcpuaffinity)
  set_tests_properties(cpuaffinity_args PROPERTIES TIMEOUT 60)
else()
  # The run ends on SIGABRT, which ctest counts as a failure whatever it
  # printed, so the shell checks the message.
  foreach(case "setcpuaffinity:+setcpuaffinity" "pemap:+pemap L0")
    string(REPLACE ":" ";" parts "${case}")
    list(GET parts 0 name)
    list(GET parts 1 flags)
    add_test(NAME cpuaffinity_args_abort_${name}
             COMMAND sh -c "$<TARGET_FILE:cpuaffinity_args> +pe 2 ${flags} 2>&1 | grep 'this build cannot bind threads'")
    set_tests_properties(cpuaffinity_args_abort_${name} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
