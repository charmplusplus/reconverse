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

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

option(RECONVERSE_BUILD_EXAMPLES "Build examples" ON)
option(RECONVERSE_BUILD_TESTS "Build tests" ON)

option(RECONVERSE_TRY_ENABLE_COMM_LCI2 "whether to enable the LCIv2 backend" ON)
option(RECONVERSE_AUTOFETCH_LCI2
        "whether to autofetch LCIv2 if LCI2 cannot be found" OFF)
set(RECONVERSE_AUTOFETCH_LCI2_TAG
<<<<<<< Updated upstream
    "948165225fa07eeb17a6e6e053117add7c04a3ee"
    CACHE STRING "The tag to fetch for LCIv2") # master branch as of 2025-06-10
=======
    "ec6af44c2819a75209893f93966e946a8c8d1970"
    CACHE STRING "The tag to fetch for LCIv2") # master branch as of 2025-04-17

option(RECONVERSE_TRY_ENABLE_COMM_LCI1 "whether to enable the LCIv1 backend" OFF)
option(RECONVERSE_AUTOFETCH_LCI1
       "whether to autofetch LCIv1 if LCI1 cannot be found" OFF)
set(RECONVERSE_AUTOFETCH_LCI1_TAG
    "ad1c43e5ff28bb2cfc8a300615f4d502b7bce31f"
    CACHE STRING "The tag to fetch for LCIv1") # master branch as of 2025-04-17
>>>>>>> Stashed changes

option(SPANTREE "whether to enable spanning tree collectives" ON)
option(CMK_SMP "whether to enable SMP support" ON)

option(RECONVERSE_ATOMIC_QUEUE "whether to use atomic queue" ON)

set(RECONVERSE_TEST_LAUNCHER
    "mpirun"
    CACHE STRING "The executable to run multiple processes in CTest")

option(ENABLE_CPU_AFFINITY "Enable cpu affinity with hwloc" OFF)

function(charm_determine_arch output input)
  if(input STREQUAL "x86_64" OR input STREQUAL "AMD64")
    set(retval "x86_64")
  elseif(input STREQUAL "i386" OR input STREQUAL "i686")
    set(retval "i386")
  elseif(input STREQUAL "powerpc" OR input MATCHES "^ppc")
    set(retval "ppc64le")
  elseif(input MATCHES "armv6" OR input MATCHES "armv7")
    set(retval "arm7")
  elseif(input MATCHES "aarch64" OR input MATCHES "arm64")
    set(retval "arm8")
  else()
    set(retval ${input})
  endif()

  set(${output} ${retval} PARENT_SCOPE)
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CHARM_OS "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
  set(CHARM_OS "win")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CHARM_OS "linux")
endif()

if(NOT ARCH)
  charm_determine_arch(ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
set(CHARM_CPU ${ARCH})

charm_determine_arch(CHARM_HOST_CPU ${CMAKE_HOST_SYSTEM_PROCESSOR})

if(NOT CHARM_CPU STREQUAL CHARM_HOST_CPU)
  if (CHARM_CPU STREQUAL "i386" AND CHARM_HOST_CPU STREQUAL "x86_64")
    message(STATUS "Attempting to cross-compile from ${CHARM_HOST_CPU} to ${CHARM_CPU}.")
    set(CMAKE_C_FLAGS -m32)
    set(CMAKE_CXX_FLAGS -m32)
    set(CMAKE_Fortran_FLAGS -m32)
  else()
    message(WARNING "Cross-compiling from ${CHARM_HOST_CPU} to ${CHARM_CPU} is not supported currently.")
  endif()
endif()

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

if(RECONVERSE_ATOMIC_QUEUE)
  add_definitions(-DATOMIC_QUEUE_ENABLED)
  message(STATUS "Atomic queue enabled")
else()
  message(STATUS "Atomic queue disabled")
endif()

add_library(reconverse $<TARGET_OBJECTS:boost-context>)
set_target_properties(reconverse PROPERTIES CXX_STANDARD 17)
add_subdirectory(include)
add_subdirectory(src)
target_link_libraries(reconverse PUBLIC Threads::Threads ${HWLOC})

# ##############################################################################
# Add Subdirectories
# ##############################################################################
add_subdirectory(boost-context)

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
