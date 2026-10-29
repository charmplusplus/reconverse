# Runs PROG with the arguments in ARGS (one string), prints everything it
# wrote to stdout and stderr, and succeeds whatever its exit status, so that a
# test can be judged on its output alone (PASS_REGULAR_EXPRESSION) even when
# the program ends in an abort or a crash. This is the portable form of
#   sh -c "prog args 2>&1; true"
# for platforms without sh.
#
# cmake -DPROG=<program> "-DARGS=<arguments>" -P run_and_capture.cmake
separate_arguments(args NATIVE_COMMAND "${ARGS}")
execute_process(COMMAND "${PROG}" ${args}
                OUTPUT_VARIABLE out
                ERROR_VARIABLE out
                RESULT_VARIABLE rc)
message("${out}")
message("exit status: ${rc}")
