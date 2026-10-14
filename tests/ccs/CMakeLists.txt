# CCS end to end: a Converse program with ++server-port answers a client
# linked against libccs-client only. Covers single-PE, broadcast (CmiReduce)
# and multicast (CmiListReduce) requests, in one process and in two. The
# launcher may be a list ("mpirun;--oversubscribe"), so it is passed to the
# script as separate arguments, not inside a quoted string.
add_reconverse_executable(ccs_server ccs_server.cpp)
add_executable(ccs_client ccs_client.cpp)
target_link_libraries(ccs_client PRIVATE ccs-client)
set(_ccs_env ${CMAKE_COMMAND} -E env
    CCS_SERVER=$<TARGET_FILE:ccs_server> CCS_CLIENT=$<TARGET_FILE:ccs_client>
    CCS_SERVER_ARGS=+pe\ 6)
add_test(NAME ccs COMMAND ${_ccs_env} sh ${CMAKE_CURRENT_SOURCE_DIR}/run_ccs.sh)
add_test(NAME ccs-multinode COMMAND ${_ccs_env} sh ${CMAKE_CURRENT_SOURCE_DIR}/run_ccs.sh
         ${RECONVERSE_TEST_LAUNCHER} -n 2)
set_tests_properties(ccs ccs-multinode PROPERTIES TIMEOUT 120
                     PASS_REGULAR_EXPRESSION "ccs_client: PASS")
