# A never-ending stream of node messages must not starve local work (#268,
# and #273 for +old-scheduler).
# A starved run hangs, so the timeout is the failure.
add_reconverse_executable(node_flood node_flood.cpp)
add_test(NAME node_flood COMMAND node_flood +pe 4)
add_test(NAME node_flood-nodeprio COMMAND node_flood +pe 4 20000 nodeprio)
add_test(NAME node_flood-old COMMAND node_flood +pe 4 20000 node +old-scheduler)
add_test(NAME node_flood-nodeprio-old
  COMMAND node_flood +pe 4 20000 nodeprio +old-scheduler)
# One PE: no other PE contends for the node priority queue's lock, which is
# what used to let local work through.
add_test(NAME node_flood-nodeprio-old-1pe
  COMMAND node_flood +pe 1 20000 nodeprio +old-scheduler)
set_tests_properties(node_flood node_flood-nodeprio node_flood-old
                     node_flood-nodeprio-old node_flood-nodeprio-old-1pe
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "local messages while")
