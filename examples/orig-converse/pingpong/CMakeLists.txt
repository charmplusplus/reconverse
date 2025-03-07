add_reconverse_executable(pingpong pingpong.C)
# Does not work for now
# add_test(NAME pingpong COMMAND pingpong +pe 4)

add_reconverse_executable(pingpong_multipairs pingpong_multipairs.C)
add_test(NAME pingpong_multipairs COMMAND pingpong_multipairs +pe 4)