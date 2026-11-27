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
  list(APPEND _cases4 race race_explicit race_handler race_data
       race_data_explicit queued_early early)
endif()
foreach(_case ${_cases4})
  add_test(NAME region_counter_local_${_case}_pe4
           COMMAND region_counter_local ${_case} +pe 4)
  set_tests_properties(region_counter_local_${_case}_pe4 PROPERTIES
    PASS_REGULAR_EXPRESSION "${_case} PASS" TIMEOUT 60)
endforeach()

# A destroy called on an application message (CmiRegionDestroy's rule):
# reports how often the destroy completed before the origin's registration
# was handled, which is allowed; it documents the behaviour.
add_test(NAME region_counter_local_destroy_order_pe4
         COMMAND region_counter_local destroy_order +pe 4)
add_test(NAME region_counter_local_destroy_order_randomized_pe4
         COMMAND region_counter_local destroy_order +pe 4 +randomized_msgq)
set_tests_properties(region_counter_local_destroy_order_pe4
  region_counter_local_destroy_order_randomized_pe4 PROPERTIES
  PASS_REGULAR_EXPRESSION "destroy_order PASS" TIMEOUT 60)

# Detach callbacks (CmiDetachTarget's done): a batch over two owner PEs, an
# attachment the owner ended and a notice it dropped, and a registration
# that several PEs share. Every build.
foreach(_case detach_done_batch detach_done_ended detach_done_shared
        done_order_new)
  add_test(NAME region_counter_local_${_case}_pe4
           COMMAND region_counter_local ${_case} +pe 4)
  set_tests_properties(region_counter_local_${_case}_pe4 PROPERTIES
    PASS_REGULAR_EXPRESSION "${_case} PASS" TIMEOUT 60)
endforeach()
add_test(NAME region_counter_local_detach_done_ended_pe1
         COMMAND region_counter_local detach_done_ended +pe 1)
set_tests_properties(region_counter_local_detach_done_ended_pe1 PROPERTIES
  PASS_REGULAR_EXPRESSION "detach_done_ended PASS" TIMEOUT 60)
if(RECONVERSE_REGION_CHECKS)
  # With each registration held 20 ms (+region_test_registration_delay), a
  # destroy on a message the origin sends right after its detach completes
  # before the registration arrives (the case asserts it does in at least
  # one round); on a message sent from the detach's done callback it never
  # does, also when the notice arrives before the registration.
  add_test(NAME region_counter_local_done_order_old_delay_pe4
           COMMAND region_counter_local done_order_old +pe 4
                   +region_test_registration_delay 20)
  add_test(NAME region_counter_local_done_order_new_delay_pe4
           COMMAND region_counter_local done_order_new +pe 4
                   +region_test_registration_delay 20)
  add_test(NAME region_counter_local_done_order_new_notice_first_pe4
           COMMAND region_counter_local done_order_new +pe 4
                   +region_test_registration_delay 20
                   +region_test_notice_first)
  set_tests_properties(region_counter_local_done_order_old_delay_pe4
    PROPERTIES PASS_REGULAR_EXPRESSION "done_order_old PASS" TIMEOUT 60)
  set_tests_properties(region_counter_local_done_order_new_delay_pe4
    region_counter_local_done_order_new_notice_first_pe4 PROPERTIES
    PASS_REGULAR_EXPRESSION "done_order_new PASS: 4 PEs; in 0 of" TIMEOUT 60)
endif()

# A destroy that waits says what for, once, after +region_destroy_report_s.
foreach(_pes 1 4)
  add_test(NAME region_counter_local_destroy_wait_pe${_pes}
           COMMAND region_counter_local destroy_wait +pe ${_pes}
                   +region_destroy_report_s 0.2)
  set_tests_properties(region_counter_local_destroy_wait_pe${_pes} PROPERTIES
    PASS_REGULAR_EXPRESSION
    "CmiRegionDestroy of region slot 0 \\(generation 0\\) of PE 0 has waited 0.2 s: 1 processes have not detached \\(process 0 first, registered, no notice yet\\).*destroy_wait PASS"
    TIMEOUT 60)
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
  region_counter_abort_test(stale_region ${_pes}
    "CmiPut: region slot [0-9]+ \\(generation [0-9]+\\) of PE 0 was destroyed; this target is stale")
  region_counter_abort_test(detached_handle ${_pes}
    "CmiPut: the attachment was detached already")
  region_counter_abort_test(double_detach ${_pes}
    "CmiDetachTarget: the attachment was detached already")
  region_counter_abort_test(bad_flags ${_pes}
    "CmiCounterCreate: unknown flags 0x100")
  region_counter_abort_test(alloc_null ${_pes}
    "CmiRegionAlloc: out is NULL")
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
