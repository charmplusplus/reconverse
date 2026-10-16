# Ping-pong or one-sender stream to PE 0 after every other PE has sent PE 0 a
# message (delivery cost vs registered producers); the tests run it briefly in
# both modes so it stays buildable.
add_reconverse_executable(pingpong_after_fanin pingpong_after_fanin.cpp)
add_test(NAME pingpong_after_fanin-pp COMMAND pingpong_after_fanin 2000 pp +pe 4)
add_test(NAME pingpong_after_fanin-stream
  COMMAND pingpong_after_fanin 2000 stream +pe 4)
set_tests_properties(pingpong_after_fanin-pp PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "ns one-way latency")
set_tests_properties(pingpong_after_fanin-stream PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "ns per message at the receiver")
