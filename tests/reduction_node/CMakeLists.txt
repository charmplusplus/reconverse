add_reconverse_executable(nodereduction reduction.cpp)
add_test(NAME node-reduction-singlenode COMMAND nodereduction +pe 7)
add_test(NAME node-reduction-multinode COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 4 $<TARGET_FILE:nodereduction> +pe 8)
# 6 processes x 2 PEs: node 5's parent in the node spanning tree is node 1,
# not node 0, and node number != PE number. With 4 processes every parent is
# node 0, which is also PE 0, so a reduction that sends to the parent's PE
# number instead of its node still passes there.
add_test(NAME node-reduction-multinode-tree COMMAND ${RECONVERSE_TEST_LAUNCHER} -n 6 $<TARGET_FILE:nodereduction> +pe 12)
