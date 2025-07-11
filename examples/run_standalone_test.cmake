# run_standalone_test.cmake

set(STANDALONE_SOURCE_DIR "${PROJECT_CURRENT_SOURCE_DIR}/standalone_project")
set(STANDALONE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/standalone_build")

file(MAKE_DIRECTORY "${STANDALONE_BINARY_DIR}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -S "${STANDALONE_SOURCE_DIR}" -B "${STANDALONE_BINARY_DIR}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "CMake configuration failed for standalone_project")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} --build "${STANDALONE_BINARY_DIR}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Build failed for standalone_project")
endif()

# Enable this once the first PR is merged into main
# execute_process(
#   COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${STANDALONE_BINARY_DIR}" --output-on-failure --verbose
#   RESULT_VARIABLE result
# )
# if(NOT result EQUAL 0)
#   message(FATAL_ERROR "Tests failed in standalone_project")
# endif()
