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

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # This is the root project
  set(RECONVERSE_ROOT_PROJECT ON)
else()
  # This is a subproject
  set(RECONVERSE_ROOT_PROJECT OFF)
endif()

option(RECONVERSE_BUILD_TESTS "Build tests" ${RECONVERSE_ROOT_PROJECT})

option(RECONVERSE_ENABLE_CUDA "Enable CUDA support (GPUDirect via LCI, cudaMemcpy for loopback)" OFF)
option(RECONVERSE_ENABLE_HIP "Enable HIP/ROCm support (GPUDirect via LCI, hipMemcpy for loopback)" OFF)

option(RECONVERSE_TRY_ENABLE_COMM_LCI2 "whether to enable the LCIv2 backend" ON)
option(RECONVERSE_AUTOFETCH_LCI2
        "whether to autofetch LCIv2 if LCI2 cannot be found" OFF)
set(RECONVERSE_AUTOFETCH_LCI2_TAG
    "1287c59c1c9dad78d6966b6af5eeeb3d7df5ec8c"
    CACHE STRING "The tag to fetch for LCIv2") # uiuc-hpc/lci master, 2026-10-24: OFI providers that need FI_CONTEXT and have 4-byte CQ data, i.e. native psm2 (#214)

option(RECONVERSE_TRY_ENABLE_COMM_LCW "whether to enable the LCW backend" ON)
option(RECONVERSE_AUTOFETCH_LCW
        "whether to autofetch LCW if LCW cannot be found" OFF)
set(RECONVERSE_AUTOFETCH_LCW_TAG
    "20689cad808bdb3af32a4707485d8e0d41fdfbe0"
    CACHE STRING "The tag to fetch for LCW") # master branch as of 2025-09-25

option(SPANTREE "whether to enable spanning tree collectives" ON) 

# Reconverse uses SMP always, but turn on this option for backwards compatibility (see pingpong_multipairs for example)
option(CMK_SMP "whether to enable SMP support" ON)
option(CMK_CPV_IS_SMP "whether to enable SMP support for cpvs" ON)
# Charm++ sets this (buildcmake --enable-shmem) to say that *it* drives the
# shared-memory pool. Reconverse always builds the pool now and turns it on
# with the +ipc runtime flag instead, so this only exists so that a Charm++
# build that passes it still configures.
option(CMK_USE_SHMEM "whether Charm++ drives the shared-memory IPC pool" OFF)

# The xpmem backend for the IPC pool. Off by default because it needs an
# xpmem installation to build against and its kernel module loaded to run;
# with it off, the pool uses POSIX shared memory, which needs neither.
# Charm++'s buildcmake --enable-xpmem passes CMK_HAS_XPMEM through, so honour
# that spelling too.
option(RECONVERSE_ENABLE_XPMEM "build the xpmem backend for shared-memory IPC" OFF)
if(CMK_HAS_XPMEM)
  set(RECONVERSE_ENABLE_XPMEM ON)
endif()
option(CMK_TASKQUEUE "use the task queue for work stealing" OFF)

option(RECONVERSE_ATOMIC_QUEUE "whether to use atomic queue" ON)

# Every message is allocated by the sending PE and freed by the receiving PE;
# glibc's malloc handles those cross-thread frees poorly (reconverse#271).
option(RECONVERSE_USE_JEMALLOC
  "Link jemalloc (libjemalloc_pic.a) into libreconverse, whole-archive, so its malloc/free replace libc's for the process. With a static libreconverse the final link must also use whole-archive."
  OFF)
set(JEMALLOC_ROOT "" CACHE PATH "jemalloc install prefix (used when RECONVERSE_USE_JEMALLOC=ON)")

set(RECONVERSE_TEST_LAUNCHER
    "mpirun"
    CACHE STRING "The executable to run multiple processes in CTest")

find_package(HWLOC)
option(RECONVERSE_ENABLE_CPU_AFFINITY "Enable cpu affinity with hwloc" ${HWLOC_FOUND})

# This option defaults to OFF whenever hwloc is absent, which silently drops
# +setcpuaffinity, +pemap and +showcpuaffinity from the build. The only symptom
# used to appear much later, at runtime, as an unrelated-looking complaint about
# an unparsed '+' argument. Say plainly at configure time which way it went.
if(RECONVERSE_ENABLE_CPU_AFFINITY AND NOT HWLOC_FOUND)
  message(FATAL_ERROR
    "RECONVERSE_ENABLE_CPU_AFFINITY=ON was requested but hwloc was not found. "
    "Install hwloc (libhwloc-dev on Debian/Ubuntu, hwloc-devel on RHEL, "
    "'brew install hwloc' on macOS) or point CMake at it with "
    "-DHWLOC_ROOT=<prefix>.")
elseif(RECONVERSE_ENABLE_CPU_AFFINITY)
  message(STATUS "CPU affinity enabled: +setcpuaffinity, +pemap and "
                 "+showcpuaffinity are available")
else()
  message(WARNING
    "hwloc was not found, so CPU affinity is DISABLED: +setcpuaffinity, +pemap "
    "and +showcpuaffinity will be accepted but do nothing. Install hwloc "
    "(libhwloc-dev on Debian/Ubuntu, hwloc-devel on RHEL, 'brew install hwloc' "
    "on macOS) and reconfigure to enable them.")
endif()

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

# Windows threads are part of the system, so there is no thread library to
# find; FindThreads, run from Cygwin for a Charm++ build, looks for pthreads
# and fails.
if(NOT CHARM_OS STREQUAL "win")
  find_package(Threads REQUIRED)
endif()

if(RECONVERSE_ENABLE_XPMEM)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_XPMEM QUIET xpmem)
  endif()
  find_path(XPMEM_INCLUDE_DIR NAMES xpmem.h
            HINTS ${PC_XPMEM_INCLUDE_DIRS} ${XPMEM_ROOT}/include
                  /opt/xpmem/include)
  find_library(XPMEM_LIBRARY NAMES xpmem
               HINTS ${PC_XPMEM_LIBRARY_DIRS} ${XPMEM_ROOT}/lib64
                     ${XPMEM_ROOT}/lib /opt/xpmem/lib64)
  if(NOT XPMEM_INCLUDE_DIR OR NOT XPMEM_LIBRARY)
    message(FATAL_ERROR
      "RECONVERSE_ENABLE_XPMEM=ON was requested but xpmem was not found. "
      "Point CMake at it with -DXPMEM_ROOT=<prefix> (the Cray systems that "
      "ship it install it in /opt/xpmem), or leave the option off to use "
      "POSIX shared memory for IPC instead.")
  endif()
  set(CMK_HAS_XPMEM 1)
  message(STATUS "xpmem IPC backend enabled: ${XPMEM_LIBRARY}")
else()
  set(CMK_HAS_XPMEM 0)
endif()

if(RECONVERSE_ENABLE_CUDA)
  find_package(CUDAToolkit REQUIRED)
  set(CMK_CUDA ON)
endif()

if(RECONVERSE_ENABLE_HIP)
  find_package(hip REQUIRED CONFIG)
  set(CMK_HIP ON)
endif()

if(RECONVERSE_ATOMIC_QUEUE)
  add_definitions(-DATOMIC_QUEUE_ENABLED)
  message(STATUS "Atomic queue enabled")
else()
  message(STATUS "Atomic queue disabled")
endif()

add_library(reconverse $<TARGET_OBJECTS:boost-context>)
set_target_properties(reconverse PROPERTIES CXX_STANDARD 17)
# Consumers must compile against converse.h as C++17 too (thread_local,
# constexpr, std::atomic in the header); without this a consumer on Apple
# clang is compiled as gnu++98 and the header fails.
target_compile_features(reconverse PUBLIC cxx_std_17)
add_subdirectory(include)
add_subdirectory(src)
if(TARGET Threads::Threads)
  target_link_libraries(reconverse PUBLIC Threads::Threads)
endif()
# Stack traces (backtrace.cpp) name functions inside the executable only if
# its symbols are exported; on Linux that takes -rdynamic at the final link.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(reconverse INTERFACE -rdynamic)
endif()
# backtrace.cpp calls dladdr, which lives in libdl before glibc 2.34.
target_link_libraries(reconverse PUBLIC ${CMAKE_DL_LIBS})
# Rank 0 runs on the main thread, whose stack size is fixed in the executable
# header. Windows defaults it to 1 MB; give it the 8 MB the other PE threads
# get (CmiStartThreads) and that Linux and macOS main threads have. It is
# reserved address space, committed only as the stack grows.
#
# /DEBUG is the counterpart of -rdynamic above: it writes a PDB beside the
# executable, which is what lets backtrace.cpp name the executable's own
# functions. It does not change the generated code, but it does switch the
# linker's dead-code and identical-code folding off unless they are asked for.
if(MSVC)
  target_link_options(reconverse INTERFACE /STACK:8388608 /DEBUG
                      $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
endif()

if(RECONVERSE_ENABLE_XPMEM)
  target_include_directories(reconverse PRIVATE ${XPMEM_INCLUDE_DIR})
  target_link_libraries(reconverse PUBLIC ${XPMEM_LIBRARY})
endif()

if(RECONVERSE_ENABLE_CUDA)
  target_link_libraries(reconverse PUBLIC CUDA::cudart)
endif()

if(RECONVERSE_ENABLE_HIP)
  target_link_libraries(reconverse PUBLIC hip::host)
endif()

if(RECONVERSE_ROOT_PROJECT)
  # When building standalone, set CMK_OPTIMIZE based on build type.
  # Can be overridden on the cmake command line with -DCMK_OPTIMIZE=1.
  if(NOT DEFINED CMK_OPTIMIZE)
    if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
      set(CMK_OPTIMIZE 1)
    else()
      set(CMK_OPTIMIZE 0)
    endif()
  endif()
  target_compile_definitions(reconverse PUBLIC CMK_OPTIMIZE=${CMK_OPTIMIZE})
endif()

if(RECONVERSE_ENABLE_CPU_AFFINITY)
  target_link_libraries(reconverse PUBLIC HWLOC::HWLOC)
endif()

if(RECONVERSE_USE_JEMALLOC)
  find_library(JEMALLOC_PIC_LIBRARY NAMES jemalloc_pic
               HINTS ${JEMALLOC_ROOT} PATH_SUFFIXES lib lib64)
  if(NOT JEMALLOC_PIC_LIBRARY)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(JEMALLOC_PC QUIET jemalloc)
      if(JEMALLOC_PC_FOUND)
        find_library(JEMALLOC_PIC_LIBRARY NAMES jemalloc_pic
                     HINTS ${JEMALLOC_PC_LIBRARY_DIRS} NO_DEFAULT_PATH)
      endif()
    endif()
  endif()
  if(NOT JEMALLOC_PIC_LIBRARY)
    message(FATAL_ERROR
      "RECONVERSE_USE_JEMALLOC=ON but libjemalloc_pic.a was not found "
      "(JEMALLOC_ROOT='${JEMALLOC_ROOT}'). Build jemalloc with "
      "'./configure --prefix=<prefix> && make -j && make install' and pass "
      "-DJEMALLOC_ROOT=<prefix>.")
  endif()
  message(STATUS "Linking jemalloc into reconverse: ${JEMALLOC_PIC_LIBRARY}")
  if(APPLE)
    target_link_libraries(reconverse PRIVATE "-Wl,-force_load,${JEMALLOC_PIC_LIBRARY}")
  else()
    target_link_libraries(reconverse PRIVATE
      "-Wl,--whole-archive" "${JEMALLOC_PIC_LIBRARY}" "-Wl,--no-whole-archive"
      ${CMAKE_DL_LIBS} m)
  endif()
  target_compile_definitions(reconverse PRIVATE RECONVERSE_USE_JEMALLOC)
endif()

if(NOT RECONVERSE_ROOT_PROJECT)
  # This project is being fetched as a subproject by another CMake project.
  # Define an alias to mimic find_package behavior and ensure consistent usage.
  add_library(reconverse::reconverse ALIAS reconverse)
endif()

# ##############################################################################
# Install (library + public headers). Previously only test executables were
# installed. Internal queue templates go to a separate subdirectory so a
# consumer can opt into them explicitly.
# ##############################################################################
include(GNUInstallDirs)
install(TARGETS reconverse
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/src/converse_config.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/queue.h src/concurrentqueue.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/reconverse-internal)

# ##############################################################################
# Add Subdirectories
# ##############################################################################
add_subdirectory(boost-context)

if(RECONVERSE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
