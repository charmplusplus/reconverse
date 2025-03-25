cmake_minimum_required(VERSION 3.12)
project(
  reconverse
  VERSION 0.1.0
  LANGUAGES CXX
  DESCRIPTION
    "Simplified re-implementation of Converse: a communication and scheduling layer, and user-level threads, used by Charm++"
  HOMEPAGE_URL "https://github.com/charmplusplus/reconverse")

enable_testing()

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

# ##############################################################################
# Add Library
# ##############################################################################

find_package(Threads REQUIRED)

add_library(reconverse)
set_target_properties(reconverse PROPERTIES CXX_STANDARD 11)
target_include_directories(reconverse PUBLIC .)
target_sources(reconverse PRIVATE conv-conds.C convcore.C queue.C scheduler.C
                                  comm_backend/comm_backend_internal.C)
target_link_libraries(reconverse PUBLIC Threads::Threads)
add_subdirectory(comm_backend)

# ##############################################################################
# Communication Backends
# ##############################################################################

# LCI1
if(RECONVERSE_TRY_ENABLE_COMM_LCI1)
  # Try to find LCIv1 externally
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
  endif()

  if(NOT RECONVERSE_ENABLE_COMM_LCI1 AND RECONVERSE_AUTOFETCH_LCI1)
    if(FETCHCONTENT_SOURCE_DIR_LCI)
      message(
        STATUS
          "LCI will be include through FetchContent from ${FETCHCONTENT_SOURCE_DIR_LCI}"
      )
    else()
      message(
        STATUS
          "LCI will be include throught FetchContent from GIT_TAG ${RECONVERSE_AUTOFETCH_LCI1_TAG}"
      )
    endif()
    include(FetchContent)
    FetchContent_Declare(
      lci
      GIT_REPOSITORY https://github.com/uiuc-hpc/lci.git
      GIT_TAG ${RECONVERSE_AUTOFETCH_LCI1_TAG})
    FetchContent_MakeAvailable(lci)
    set(RECONVERSE_ENABLE_COMM_LCI1 ON)
  endif()

  if(RECONVERSE_ENABLE_COMM_LCI1)
    target_link_libraries(reconverse PRIVATE LCI::LCI)
    target_sources(reconverse PRIVATE comm_backend/lci1/comm_backend_lci1.C)
  else()
    message(
      WARNING
        "LCI is not found. The LCI backend is not enabled. Try setting RECONVERSE_AUTOFETCH_LCI1=ON to fetch LCIv1 automatically."
    )
  endif()
endif()

# ##############################################################################
# Configure
# ##############################################################################
configure_file(converse_config.h.in ${CMAKE_BINARY_DIR}/converse_config.h)
target_include_directories(
  reconverse PUBLIC $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
                    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# ##############################################################################
# Add Subdirectories
# ##############################################################################

if(RECONVERSE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(RECONVERSE_BUILD_TESTS)
  add_subdirectory(test)
endif()
