# Many rendezvous-size sends from one handler, between two processes, with a
# small LCI packet pool and few preposted receives, so that the sends could take
# every packet receives are posted from (issue #257; see rdv_flood.cpp). Hangs
# if they can: before the fix it hung in 7 of 7 runs on Delta, on one node and
# across two.
add_reconverse_executable(rdv_flood rdv_flood.cpp)
add_test(NAME rdv_flood-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:rdv_flood> 4000 16384 +pe 4)
set_tests_properties(rdv_flood-multinode PROPERTIES TIMEOUT 60
  ENVIRONMENT "LCI_ATTR_NPACKETS=2048;LCI_ATTR_NET_MAX_RECVS=256"
  PASS_REGULAR_EXPRESSION "rdv_flood: .*PASS")
