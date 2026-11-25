# Origins in every locality: the owner's PE, its process's other PEs, and
# the other process's PEs (with two processes).
add_reconverse_executable(region_counter_mixed region_counter_mixed.cpp)

set(_cases mixed signal get nocounter reuse)
set(_tests)
foreach(_case ${_cases})
  add_test(NAME region_counter_mixed_${_case}_1proc
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 1
            $<TARGET_FILE:region_counter_mixed> ${_case} +pe 4)
  add_test(NAME region_counter_mixed_${_case}_2proc
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
            $<TARGET_FILE:region_counter_mixed> ${_case} +pe 4)
  set_tests_properties(region_counter_mixed_${_case}_1proc
    region_counter_mixed_${_case}_2proc PROPERTIES
    PASS_REGULAR_EXPRESSION "${_case} PASS" TIMEOUT 120)
endforeach()

# The ways data travels between processes, on the case that uses them all.
# Every put a message: no one-sided support.
add_test(NAME region_counter_mixed_nordma
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          mixed +pe 4 +nordma +region_verbose)
set_tests_properties(region_counter_mixed_nordma PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "in one message with its signal.*mixed PASS")
# The other processes through the shared-memory pool: messages there, puts
# still over the network.
add_test(NAME region_counter_mixed_ipc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          mixed +pe 4 +ipc)
set_tests_properties(region_counter_mixed_ipc PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "mixed PASS")
if(RECONVERSE_ENABLE_COMM_LCI2)
  # Puts land on the device their region was registered on.
  add_test(NAME region_counter_mixed_2dev
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            mixed +pe 8 +lci_ndevices 2 rounds=400)
  # The backend without put with signal: every put a message, gets one-sided.
  add_test(NAME region_counter_mixed_nosignal
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            mixed +pe 4 +lci_no_put_signal +region_verbose)
  # The emulated put with signal (cxi's path): puts up to LCI's eager limit
  # as messages, larger ones as puts; then every put as a put; then every put
  # as a message.
  add_test(NAME region_counter_mixed_emulated
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            mixed +pe 4 +lci_put_signal_emulate +region_verbose)
  add_test(NAME region_counter_mixed_emulated_puts
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            mixed +pe 4 +lci_put_signal_emulate +put_message_max_bytes 0
            +region_verbose)
  add_test(NAME region_counter_mixed_emulated_messages
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            mixed +pe 4 +lci_put_signal_emulate +put_message_max_bytes 65536
            +region_verbose)
  set_tests_properties(region_counter_mixed_2dev PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "mixed PASS")
  set_tests_properties(region_counter_mixed_nosignal
    region_counter_mixed_emulated_messages PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "in one message with its signal.*mixed PASS"
    FAIL_REGULAR_EXPRESSION "as a put with signal")
  set_tests_properties(region_counter_mixed_emulated_puts PROPERTIES
    TIMEOUT 120
    PASS_REGULAR_EXPRESSION "as a put with signal.*mixed PASS"
    FAIL_REGULAR_EXPRESSION "in one message with its signal")
  # both ways: the first rounds' puts, 8 to 4096 bytes, fit LCI's eager
  # limit (8116 bytes at the default 8 KiB packet); the sixth's 8200 do not
  set_tests_properties(region_counter_mixed_emulated PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION
    "a 8-byte CmiPut to process 0 travels in one message with its signal.*a 8200-byte CmiPut to process 0 travels as a put with signal.*mixed PASS")
  # At the default threshold, emulated: a put of exactly the limit goes in a
  # message, one of a byte more as a put with signal.
  add_test(NAME region_counter_mixed_threshold
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            threshold +pe 4 +lci_put_signal_emulate +region_verbose rounds=200)
  set_tests_properties(region_counter_mixed_threshold PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION
    "puts of ([0-9]+) and [0-9]+ bytes.*CmiPut to process 0 travels in one message with its signal.*CmiPut to process 0 travels as a put with signal.*threshold PASS"
    FAIL_REGULAR_EXPRESSION "a 0-byte CmiPut")
endif()

# Readiness errors that only the owner's process can see (design 11): the
# CTest entries match the abort, since the status is a signal.
function(region_counter_early_test name pattern)
  add_test(NAME region_counter_early_${name}
    COMMAND sh -c "${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed> ${name} +pe 2 2>&1; true")
  set_tests_properties(region_counter_early_${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${pattern}"
    FAIL_REGULAR_EXPRESSION "region_counter_mixed [a-z_]+ FAIL" TIMEOUT 60)
endfunction()
if(NOT WIN32)
  # a signal through a target of a destroyed counter, from another process;
  # the origin's process sees it in an error-checking build, the owner's in
  # any
  region_counter_early_test(stale_remote
    "the target is stale|destroyed this target's counter and detached this process")
  if(RECONVERSE_REGION_CHECKS)
    # a remote origin's second put before the owner re-armed
    region_counter_early_test(count_remote
      "a contribution arrived after the round fired and before the owner re-armed it")
  endif()
endif()

# CmiPut ping-pong from unregistered memory (open question 11): a short run
# here, so that it keeps building and running.
add_reconverse_executable(region_put_bench region_put_bench.cpp)
add_test(NAME region_put_bench
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_put_bench>
          fresh iters=5 max=262144 +pe 2)
set_tests_properties(region_put_bench PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "262144 B: .*region_put_bench PASS")
