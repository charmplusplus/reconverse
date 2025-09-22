add_reconverse_executable(pingpong pingpong.cpp)
add_test(NAME pingpong COMMAND pingpong +pe 2)

add_reconverse_executable(pingpong_multipairs pingpong_multipairs.cpp)
add_test(NAME pingpong_multipairs COMMAND pingpong_multipairs +pe 4)
