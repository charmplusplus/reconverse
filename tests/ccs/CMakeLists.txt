# CCS end to end: a Converse program with ++server-port answers a client
# linked against libccs-client only. Covers single-PE, broadcast (CmiReduce)
# and multicast (CmiListReduce) requests, in one process and in two.
add_reconverse_executable(ccs_server ccs_server.cpp)
add_executable(ccs_client ccs_client.cpp)
target_link_libraries(ccs_client PRIVATE ccs-client)
add_test(NAME ccs COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_ccs.sh
         $<TARGET_FILE:ccs_server> $<TARGET_FILE:ccs_client> "" +pe 6)
add_test(NAME ccs-multinode COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_ccs.sh
         $<TARGET_FILE:ccs_server> $<TARGET_FILE:ccs_client>
         "${RECONVERSE_TEST_LAUNCHER} -n 2" +pe 6)
set_tests_properties(ccs ccs-multinode PROPERTIES TIMEOUT 120
                     PASS_REGULAR_EXPRESSION "ccs_client: PASS")
