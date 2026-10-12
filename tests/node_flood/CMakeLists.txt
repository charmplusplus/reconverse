# A never-ending stream of node messages must not starve local work (#268).
# A starved run hangs, so the timeout is the failure.
add_reconverse_executable(node_flood node_flood.cpp)
add_test(NAME node_flood COMMAND node_flood +pe 4)
add_test(NAME node_flood-nodeprio COMMAND node_flood +pe 4 20000 nodeprio)
set_tests_properties(node_flood node_flood-nodeprio
  PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "local messages while")
