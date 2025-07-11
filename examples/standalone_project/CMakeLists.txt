cmake_minimum_required(VERSION 3.12)
project(reconverse_project LANGUAGES CXX)
 
# Fetch Reconverse from GitHub
# This is a CMake feature that allows you to download and build reconverse as part of your project.
include(FetchContent)
FetchContent_Declare(
  reconverse
  GIT_REPOSITORY https://github.com/charmplusplus/reconverse.git
  GIT_TAG main
)
message(STATUS "Fetching Reconverse from https://github.com/charmplusplus/reconverse.git...")
FetchContent_MakeAvailable(reconverse)
 
# Add a new executable
# Temporary solution
add_executable(standalone_broadcast broadcast.cpp)
target_link_libraries(standalone_broadcast PRIVATE reconverse)
# Switch to this once the first PR is merged into main
# add_executable(broadcast broadcast.cpp)
# target_link_libraries(broadcast PRIVATE reconverse::reconverse)
# Optionally, add a test
# add_test(NAME standalone-broadcast COMMAND broadcast +pe 4)
