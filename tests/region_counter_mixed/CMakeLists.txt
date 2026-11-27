# Origins in every locality: the owner's PE, its process's other PEs, and
# the other process's PEs (with two processes).
add_reconverse_executable(region_counter_mixed region_counter_mixed.cpp)

set(_cases mixed signal get nocounter reuse zero)
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

# Attach, transfer and detach cycles with the transfers in flight at the
# detach, two processes of four PEs: the notice that waits for the last
# completion. Error-checking builds widen the race with a delay in each
# completion; production builds run without one.
if(RECONVERSE_REGION_CHECKS)
  set(_delay +region_test_completion_delay 50 rounds=2000)
else()
  set(_delay rounds=1000)
endif()
add_test(NAME region_counter_mixed_detach_stress_2proc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          detach_stress +pe 8 ${_delay})
add_test(NAME region_counter_mixed_detach_stress_1proc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 1 $<TARGET_FILE:region_counter_mixed>
          detach_stress +pe 4 rounds=500)
set_tests_properties(region_counter_mixed_detach_stress_2proc
  region_counter_mixed_detach_stress_1proc PROPERTIES
  PASS_REGULAR_EXPRESSION "detach_stress PASS" TIMEOUT 120)

# QD counts a contribution to another process's counter with no action as
# created at issue and processed where it lands: through a put with signal
# (or a message where there is none) and a CmiSignal, then all as messages.
add_test(NAME region_counter_mixed_qd_remote_2proc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          qd_remote +pe 2)
add_test(NAME region_counter_mixed_qd_remote_nordma
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          qd_remote +pe 2 +nordma)
set_tests_properties(region_counter_mixed_qd_remote_2proc
  region_counter_mixed_qd_remote_nordma PROPERTIES
  PASS_REGULAR_EXPRESSION "qd_remote PASS.*QD balanced at 8" TIMEOUT 60)

# The ways data travels between processes, on the case that uses them all.
# Every put a message: no one-sided support.
add_test(NAME region_counter_mixed_nordma
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          mixed +pe 4 +nordma +region_verbose)
set_tests_properties(region_counter_mixed_nordma PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "in one message with its signal.*mixed PASS")
# The get fallback (a request and a reply) and puts without a counter in
# messages: no one-sided support.
add_test(NAME region_counter_mixed_get_nordma
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          get +pe 4 +nordma)
add_test(NAME region_counter_mixed_nocounter_nordma
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          nocounter +pe 4 +nordma)
set_tests_properties(region_counter_mixed_get_nordma PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "get PASS")
set_tests_properties(region_counter_mixed_nocounter_nordma PROPERTIES
  TIMEOUT 120 PASS_REGULAR_EXPRESSION "nocounter PASS")
# The other processes through the shared-memory pool: messages there, puts
# still over the network. The origins report how many messages they sent
# through the pool, which must not be none.
add_test(NAME region_counter_mixed_ipc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
          mixed +pe 4 +ipc)
set_tests_properties(region_counter_mixed_ipc PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION
  "mixed PASS: .*; [1-9][0-9]* messages through the IPC pool")
if(RECONVERSE_ENABLE_COMM_LCI2)
  # Two devices, ranks 0-1 of each process on device 0 and ranks 2-3 on
  # device 1: a one-sided transfer goes on the device its region was
  # registered on (#203). mixed with the owner on PE 0 (device 0), then
  # mixed and nocounter with the owner on the last PE (device 1), and get,
  # where every PE owns a region.
  add_test(NAME region_counter_mixed_2dev
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            mixed +pe 8 +lci_ndevices 2 rounds=400)
  foreach(_case mixed nocounter get)
    add_test(NAME region_counter_mixed_2dev_${_case}_last
      COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
              $<TARGET_FILE:region_counter_mixed> ${_case} +pe 8
              +lci_ndevices 2 owner=last rounds=400)
    set_tests_properties(region_counter_mixed_2dev_${_case}_last PROPERTIES
      TIMEOUT 120 PASS_REGULAR_EXPRESSION "${_case} PASS")
  endforeach()
  # Puts without a counter in messages where the backend has no put with
  # signal.
  add_test(NAME region_counter_mixed_nocounter_nosignal
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            nocounter +pe 4 +lci_no_put_signal)
  set_tests_properties(region_counter_mixed_nocounter_nosignal PROPERTIES
    TIMEOUT 120 PASS_REGULAR_EXPRESSION "nocounter PASS")
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
  # both ways: the first rounds' puts, 8 to 4096 bytes, fit one eager
  # message (LCI's 8116 bytes at the default 8 KiB packet, less the 72-byte
  # header); the sixth's 8200 do not
  set_tests_properties(region_counter_mixed_emulated PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION
    "a 8-byte CmiPut to process 0 travels in one message with its signal.*a 8200-byte CmiPut to process 0 travels as a put with signal.*mixed PASS")
  # At the default threshold, emulated: a put of exactly the limit goes in
  # one eager message (header included), one of a byte more as a put with
  # signal.
  add_test(NAME region_counter_mixed_threshold
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_counter_mixed>
            threshold +pe 4 +lci_put_signal_emulate +region_verbose rounds=200)
  set_tests_properties(region_counter_mixed_threshold PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION
    "puts of ([0-9]+) and [0-9]+ bytes.*CmiPut to process 0 travels in one message with its signal \\([0-9]+ bytes, eager\\).*CmiPut to process 0 travels as a put with signal.*threshold PASS"
    FAIL_REGULAR_EXPRESSION "a 0-byte CmiPut|rendezvous")
endif()

if(RECONVERSE_ENABLE_COMM_LCW)
  # The LCW backend (+lcw_backend auto: LCW over LCI2 where LCW has it): no
  # one-sided transfers, so every put and get to another process is a
  # message, and CmiSignal is a Converse message, since LCW takes one
  # active-message handler.
  foreach(_case mixed signal get nocounter qd_remote)
    add_test(NAME region_counter_mixed_lcw_${_case}
      COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2
              $<TARGET_FILE:region_counter_mixed> ${_case} +pe 4 +backend lcw
              +lcw_backend auto rounds=200)
    set_tests_properties(region_counter_mixed_lcw_${_case} PROPERTIES
      TIMEOUT 120 PASS_REGULAR_EXPRESSION "${_case} PASS")
  endforeach()
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
  # a signal through a target of a destroyed counter, from another process:
  # the origin's process refuses it once the detach request has run, and
  # the owner's process otherwise
  region_counter_early_test(stale_remote
    "the target is stale|destroyed this target's counter and detached this process")
  # an origin that keeps putting after the owner's CMI_DESTROY_DETACH_ALL;
  # refused in every build
  region_counter_early_test(detach_all_remote
    "CmiPut: the owner, PE 0, destroyed this target's region and detached this process")
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
# Source buffers whose addresses change: in turn from a few, and new pages
# every put.
add_test(NAME region_put_bench_rotate
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_put_bench>
          rotate count=4 iters=5 max=262144 +pe 2)
set_tests_properties(region_put_bench_rotate PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "rotate4 262144 B: .*region_put_bench PASS")
if(NOT WIN32)
  add_test(NAME region_put_bench_mmap
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:region_put_bench>
            mmap iters=5 max=262144 +pe 2)
  set_tests_properties(region_put_bench_mmap PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "mmap 262144 B: .*fill.*region_put_bench PASS")
endif()
