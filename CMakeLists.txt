cmake_minimum_required(VERSION 3.12)
project(
  reconverse
  VERSION 0.1.0
  LANGUAGES CXX
  DESCRIPTION
    "Simplified re-implementation of Converse: a communication and scheduling layer, and user-level threads, used by Charm++"
  HOMEPAGE_URL "https://github.com/charmplusplus/reconverse")

cmake_policy(SET CMP0079 NEW)

enable_language(CXX)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules")

include(AddReconverse)

# ##############################################################################
# General Options
# ##############################################################################

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

option(RECONVERSE_BUILD_EXAMPLES "Build examples" ON)
option(RECONVERSE_BUILD_TESTS "Build tests" ON)

# ##############################################################################
# Add Library
# ##############################################################################

find_package(Threads REQUIRED)

add_library(reconverse)
set_target_properties(reconverse PROPERTIES CXX_STANDARD 11)
target_include_directories(reconverse PUBLIC .)
target_sources(reconverse PRIVATE conv-conds.C convcore.C queue.C scheduler.C threads.C)
target_link_libraries(reconverse PUBLIC Threads::Threads)

# ##############################################################################
# Add Subdirectories
# ##############################################################################

if(RECONVERSE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(RECONVERSE_BUILD_TESTS)
  add_subdirectory(test)
endif()
