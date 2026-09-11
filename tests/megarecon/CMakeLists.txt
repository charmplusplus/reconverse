add_reconverse_executable(megarecon megarecon.cpp handler.cpp ringsimple.cpp
  bigmsg.cpp broadc.cpp multicast.cpp multisend.cpp deadlock.cpp
  reduction.cpp nodereduction.cpp)
add_test(NAME megarecon COMMAND megarecon +pe 4)
add_test(NAME megarecon-onepe COMMAND megarecon +pe 1)
add_test(NAME megarecon-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 2 $<TARGET_FILE:megarecon> +pe 4)
