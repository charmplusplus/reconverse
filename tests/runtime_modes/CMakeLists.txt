add_reconverse_executable(runtime_modes runtime_modes.cpp)

# Two processes on one host, two PEs each, so that ranks other than 0 take
# part; +nordma selects the copy-based Direct API (the path Charm++ falls back
# to on a backend without RMA); +pemap uses hwloc logical PU indices so the
# map is valid inside any cpuset a batch system hands out. The binding itself
# is only implemented (and only checked) on Linux.
set(RUNTIME_MODES_ARGS +pe 4 +nordma +pemap L0-3)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND RECONVERSE_ENABLE_CPU_AFFINITY)
  list(APPEND RUNTIME_MODES_ARGS -expect-bound)
endif()
add_test(NAME runtime_modes
         COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:runtime_modes> ${RUNTIME_MODES_ARGS})
set_tests_properties(runtime_modes PROPERTIES TIMEOUT 120)
