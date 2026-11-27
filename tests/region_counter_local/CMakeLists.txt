add_reconverse_executable(region_counter_local region_counter_local.cpp)

# Every case in one process: one PE (origins on the owner's PE) and four.
set(_cases signal put rounds auto local get nocounter combos actions teardown
    batch errors qd early_local queued auto_handler)
foreach(_case ${_cases})
  foreach(_pes 1 4)
    add_test(NAME region_counter_local_${_case}_pe${_pes}
             COMMAND region_counter_local ${_case} +pe ${_pes})
    set_tests_properties(region_counter_local_${_case}_pe${_pes} PROPERTIES
      PASS_REGULAR_EXPRESSION "${_case} PASS" TIMEOUT 60)
  endforeach()
endforeach()
# Cases that need other PEs.
set(_cases4 attach attach_all)
if(NOT RECONVERSE_REGION_CHECKS)
  # early contributions are errors where the checks are on (design 4.2)
  list(APPEND _cases4 race race_explicit race_handler queued_early early)
endif()
foreach(_case ${_cases4})
  add_test(NAME region_counter_local_${_case}_pe4
           COMMAND region_counter_local ${_case} +pe 4)
  set_tests_properties(region_counter_local_${_case}_pe4 PROPERTIES
    PASS_REGULAR_EXPRESSION "${_case} PASS" TIMEOUT 60)
endforeach()

# Cases that end in CmiAbort: the output decides, since the status is a
# signal, and the pattern is a message only the library prints.
function(region_counter_abort_test name pes pattern)
  set(_test region_counter_local_${name}_pe${pes})
  string(JOIN " " _extra ${ARGN})
  if(WIN32)
    add_test(NAME ${_test}
      COMMAND ${CMAKE_COMMAND} -DPROG=$<TARGET_FILE:region_counter_local>
              "-DARGS=${name} +pe ${pes} ${_extra}"
              -P ${PROJECT_SOURCE_DIR}/tests/run_and_capture.cmake)
  else()
    add_test(NAME ${_test}
      COMMAND sh -c "$<TARGET_FILE:region_counter_local> ${name} +pe ${pes} ${_extra} 2>&1; true")
  endif()
  set_tests_properties(${_test} PROPERTIES PASS_REGULAR_EXPRESSION "${pattern}"
    FAIL_REGULAR_EXPRESSION "region_counter_local [a-z_]+ FAIL" TIMEOUT 30)
endfunction()

foreach(_pes 1 4)
  # every build
  region_counter_abort_test(rearm_early ${_pes}
    "re-armed before the round's action started")
  region_counter_abort_test(stale ${_pes} "was destroyed; this target is stale")
  region_counter_abort_test(table_limit ${_pes}
    "already has 3 counters, the limit" +counter_table 3)
  region_counter_abort_test(detached ${_pes}
    "destroyed this target's counter and detached this process")
  region_counter_abort_test(late_attach ${_pes}
    "destroyed this target's region and detached this process")
  if(RECONVERSE_REGION_CHECKS)
    region_counter_abort_test(early ${_pes}
      "a contribution arrived after the round fired and before the owner re-armed it")
    region_counter_abort_test(round_skip ${_pes}
      "arrived while the owner had armed 1 rounds")
  endif()
endforeach()
foreach(_pes 2 4)
  region_counter_abort_test(wrong_pe ${_pes}
    "the counter belongs to PE 0; call this on that PE")
  region_counter_abort_test(detach_all_put ${_pes}
    "CmiPut: the owner, PE 0, destroyed this target's region and detached this process")
endforeach()
