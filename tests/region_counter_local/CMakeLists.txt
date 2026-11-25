add_reconverse_executable(region_counter_local region_counter_local.cpp)

# Every case in one process: one PE (origins on the owner's PE) and four.
set(_cases signal put rounds auto local get nocounter combos actions teardown
    batch errors qd)
foreach(_case ${_cases})
  foreach(_pes 1 4)
    add_test(NAME region_counter_local_${_case}_pe${_pes}
             COMMAND region_counter_local ${_case} +pe ${_pes})
    set_tests_properties(region_counter_local_${_case}_pe${_pes} PROPERTIES
      PASS_REGULAR_EXPRESSION "${_case} PASS" TIMEOUT 60)
  endforeach()
endforeach()

# Cases that end in CmiAbort: the output decides, since the status is a signal.
function(region_counter_abort_test name pattern)
  if(WIN32)
    add_test(NAME region_counter_local_${name}
      COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:region_counter_local>
              "-DARGS=${name} +pe 1"
              -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
  else()
    add_test(NAME region_counter_local_${name}
      COMMAND sh -c "$<TARGET_FILE:region_counter_local> ${name} +pe 1 2>&1; true")
  endif()
  set_tests_properties(region_counter_local_${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${pattern}" TIMEOUT 30)
endfunction()

region_counter_abort_test(rearm_early "re-armed before the round's action started")
region_counter_abort_test(stale "destroyed")
if(RECONVERSE_ERROR_CHECKING)
  # an early contribution is an error where the checks are on (design 4.2)
  region_counter_abort_test(early
    "a contribution arrived after the round fired and before the owner re-armed it")
  region_counter_abort_test(round_skip "arrived while the owner had armed 1 rounds")
else()
  add_test(NAME region_counter_local_early COMMAND region_counter_local early +pe 1)
  set_tests_properties(region_counter_local_early PROPERTIES
    PASS_REGULAR_EXPRESSION "early PASS" TIMEOUT 30)
endif()
