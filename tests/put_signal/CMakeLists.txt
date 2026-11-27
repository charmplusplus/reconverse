# Put with signal (CmiIssueRputSignal): many concurrent signalled puts, each
# payload checked by the handler when its signal arrives, two kinds of signal,
# and puts to several PEs of each process. One process takes the copy within
# a process; two processes the network.
add_reconverse_executable(put_signal put_signal.cpp)
add_test(NAME put_signal-1proc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 1 $<TARGET_FILE:put_signal> +pe 2)
add_test(NAME put_signal-2proc
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:put_signal> +pe 4)
# Converse's emulation, where CmiIsPutSignalCapable() is 0: copy-based RDMA.
add_test(NAME put_signal-nordma
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:put_signal> +pe 4
          +nordma)
# Put-and-notify latency against a message ping-pong; a short run here, so
# that it keeps building and running.
add_reconverse_executable(put_signal_pingpong put_signal_pingpong.cpp)
add_test(NAME put_signal_pingpong
  COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:put_signal_pingpong>
          +pe 2 20 5)
set_tests_properties(put_signal_pingpong PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "65536 bytes: put with signal")
set(_put_signal_tests put_signal-1proc put_signal-2proc put_signal-nordma)
if(RECONVERSE_ENABLE_COMM_LCI2)
  # A put lands on the device its destination's owner registered it on, so
  # with two devices the puts of one origin travel on both.
  add_test(NAME put_signal-2proc-2dev
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:put_signal> +pe 8
            +lci_ndevices 2 +lci_put_signal_verbose)
  # The write, then a zero-length message carrying the signal, as LCI sends it
  # on networks without write with immediate data (cxi by default). LCI
  # allocates the received message through CmiAlloc, which refuses 0 bytes.
  add_test(NAME put_signal-emulated
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:put_signal> +pe 8
            +lci_ndevices 2 +lci_put_signal_emulate +lci_put_signal_verbose)
  # The backend's put with signal off: Converse's emulation again.
  add_test(NAME put_signal-nosignal
    COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:put_signal> +pe 4
            +lci_no_put_signal +lci_put_signal_verbose)
  list(APPEND _put_signal_tests put_signal-2proc-2dev put_signal-emulated
       put_signal-nosignal)
endif()
# A zero-length signal message that reaches CmiAlloc prints "CmiAlloc: size
# <= 0" and is handed on as NULL; count that as a failure.
set_tests_properties(${_put_signal_tests} PROPERTIES TIMEOUT 120
  PASS_REGULAR_EXPRESSION "every signal PASS"
  FAIL_REGULAR_EXPRESSION "CmiAlloc: size <= 0;CmiFree: msg is nullptr")
