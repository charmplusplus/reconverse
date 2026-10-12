# Many-to-one throughput benchmark (producer contention on PE 0's queue); the
# test runs it briefly under each scheduler so it stays buildable.
add_reconverse_executable(many_to_one many_to_one.cpp)
add_test(NAME many_to_one COMMAND many_to_one +pe 4 5000)
add_test(NAME many_to_one-old COMMAND many_to_one +pe 4 5000 +old-scheduler)
set_tests_properties(many_to_one many_to_one-old
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "ns per message at the receiver")
