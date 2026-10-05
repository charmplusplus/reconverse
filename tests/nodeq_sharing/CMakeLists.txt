# Node-queue traffic vs. CmiWallTimer on other PEs (see nodeq_sharing.cpp).
# A benchmark more than a test: the ctest entry only checks that it runs.
add_reconverse_executable(nodeq_sharing nodeq_sharing.cpp)
add_test(NAME nodeq_sharing COMMAND $<TARGET_FILE:nodeq_sharing> +pe 4 -timers 2 -seconds 0.2)
set_tests_properties(nodeq_sharing PROPERTIES TIMEOUT 60)
