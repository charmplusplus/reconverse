target_include_directories(reconverse PRIVATE .)
target_sources(reconverse PRIVATE conv-conds.C convcore.C queue.C random.C 
                scheduler.C cpuaffinity.C
                comm_backend/comm_backend_internal.C cldb.none.C)
target_include_directories(
  reconverse PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_subdirectory(comm_backend)

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

# configure needs to be called after the backend has been decided
configure_file(converse_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/converse_config.h)
