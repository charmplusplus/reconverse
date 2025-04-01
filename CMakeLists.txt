cmake_minimum_required(VERSION 3.12)
project(
  reconverse
  VERSION 0.1.0
  LANGUAGES CXX
  DESCRIPTION
    "Simplified re-implementation of Converse: a communication and scheduling layer, and user-level threads, used by Charm++"
  HOMEPAGE_URL "https://github.com/charmplusplus/reconverse")

enable_testing()

cmake_policy(SET CMP0076 NEW)
cmake_policy(SET CMP0079 NEW)

enable_language(CXX)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules")

include(AddReconverse)

# ##############################################################################
# General Options
# ##############################################################################

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

option(RECONVERSE_BUILD_EXAMPLES "Build examples" ON)
option(RECONVERSE_BUILD_TESTS "Build tests" ON)

option(RECONVERSE_TRY_ENABLE_COMM_LCI1 "whether to enable the LCIv1 backend" ON)
option(RECONVERSE_AUTOFETCH_LCI1
       "whether to autofetch LCIv1 if LCI1 cannot be found" OFF)
set(RECONVERSE_AUTOFETCH_LCI1_TAG
    "470c7974c0b073026eb4180e0e739540fb487f3e"
    CACHE STRING "The tag to fetch for LCIv1") # master branch as of 2025-03-17
# option(LCW_TRY_ENABLE_COMM_LCI2 "whether to enable the LCIv2 backend" ON)

set(RECONVERSE_TEST_LAUNCHER
    "mpirun"
    CACHE STRING "The executable to run multiple processes in CTest")

option(ENABLE_CPU_AFFINITY "Enable cpu affinity with hwloc" OFF)

# ##############################################################################
# Add Library
# ##############################################################################

find_package(Threads REQUIRED)

if(ENABLE_CPU_AFFINITY)
  find_library(HWLOC NAMES libhwloc.so libhwloc.dylib PATHS /opt/local/lib /usr/lib /usr/local/lib /usr/local/Cellar/hwloc/2.12.0/lib)
  if(HWLOC)
    message(STATUS "Found hwloc: ${HWLOC}")
  else()
    message(FATAL_ERROR "Could not find libhwloc")
  endif()
endif()

add_library(reconverse)
set_target_properties(reconverse PROPERTIES CXX_STANDARD 11)
add_subdirectory(include)
add_subdirectory(src)
target_link_libraries(reconverse PUBLIC Threads::Threads ${HWLOC})

# ##############################################################################
# Communication Backends
# ##############################################################################

# ##############################################################################
# Add Subdirectories
# ##############################################################################

if(RECONVERSE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(RECONVERSE_BUILD_TESTS)
  add_subdirectory(test)
endif()

if(ENABLE_CPU_AFFINITY)
  add_definitions(-DSET_CPU_AFFINITY)
  message(STATUS "CPU affinity enabled")
else()
  message(STATUS "CPU affinity disabled")
endif()
