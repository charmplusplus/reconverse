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

option(RECONVERSE_TRY_ENABLE_COMM_LCI1 "whether to enable the LCIv1 backend" ON)
# option(LCW_TRY_ENABLE_COMM_LCI2 "whether to enable the LCIv2 backend" ON)

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

add_library(reconverse $<TARGET_OBJECTS:boost-context>)
set_target_properties(reconverse PROPERTIES CXX_STANDARD 11)
target_include_directories(reconverse PUBLIC .)
target_sources(reconverse PRIVATE conv-conds.C convcore.C queue.C scheduler.C comm_backend/comm_backend_internal.C threads.C)
target_link_libraries(reconverse PUBLIC Threads::Threads)
add_subdirectory(comm_backend)

# ##############################################################################
# Communication Backends
# ##############################################################################

# LCI1
if(RECONVERSE_TRY_ENABLE_COMM_LCI1)
  find_package(
    LCI
    CONFIG
    HINTS
    ${LCI_ROOT}
    $ENV{LCI_ROOT}
    PATH_SUFFIXES
    lib/cmake
    lib64/cmake)
  if(LCI_FOUND
    AND NOT LCI_WITH_LCT_ONLY
    AND LCI_VERSION VERSION_LESS 2.0.0)
    set(RECONVERSE_ENABLE_COMM_LCI1 ON)
    target_link_libraries(reconverse PRIVATE LCI::LCI)
    target_sources(reconverse PRIVATE comm_backend/lci1/comm_backend_lci1.C)
  else()
    message(WARNING "LCI is not found. The LCI backend is not enabled.")
  endif()
endif()

# ##############################################################################
# Configure
# ##############################################################################
configure_file(converse_config.h.in ${CMAKE_BINARY_DIR}/converse_config.h)
target_include_directories(reconverse PUBLIC $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
                                              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

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
