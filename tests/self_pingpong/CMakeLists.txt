# Self-send cost benchmark; the test runs it briefly under each scheduler so
# it stays buildable. For numbers run it by hand with +pe 1 and +pe <many>.
add_reconverse_executable(self_pingpong self_pingpong.cpp)
add_test(NAME self_pingpong COMMAND self_pingpong +pe 2 10000)
add_test(NAME self_pingpong-old COMMAND self_pingpong +pe 2 10000 +old-scheduler)
set_tests_properties(self_pingpong self_pingpong-old
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "ns per message")
