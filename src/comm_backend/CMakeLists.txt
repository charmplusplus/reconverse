target_include_directories(reconverse PRIVATE .)

# LCI2
if(RECONVERSE_TRY_ENABLE_COMM_LCI2)
  # Try to find LCIv2 externally
  find_package(
    LCI
    CONFIG
    HINTS
    ${LCI_ROOT}
    $ENV{LCI_ROOT}
    PATH_SUFFIXES
    lib/cmake
    lib64/cmake)
  if(NOT LCI_FOUND)
    message(STATUS "Existing LCI installation not found.")
  elseif(LCI_WITH_LCT_ONLY)
    message(
      STATUS
        "Existing LCI installation found, but it is only LCT."
    )
  elseif(LCI_VERSION VERSION_LESS 2.0.0)
    message(
      STATUS
        "Existing LCI installation found, but it is LCIv1."
    )
  else()
    message(STATUS "Existing LCI installation found.")
    set(RECONVERSE_ENABLE_COMM_LCI2 ON)
  endif()

  if(NOT RECONVERSE_ENABLE_COMM_LCI2 AND RECONVERSE_AUTOFETCH_LCI2)
    if(FETCHCONTENT_SOURCE_DIR_LCI)
      message(
        STATUS
          "LCI will be include through FetchContent from ${FETCHCONTENT_SOURCE_DIR_LCI}"
      )
    else()
      message(
        STATUS
          "LCI will be include throught FetchContent from GIT_TAG ${RECONVERSE_AUTOFETCH_LCI2_TAG}"
      )
    endif()
    include(FetchContent)
    FetchContent_Declare(
      lci
      GIT_REPOSITORY https://github.com/uiuc-hpc/lci.git
      GIT_TAG ${RECONVERSE_AUTOFETCH_LCI2_TAG})
    FetchContent_MakeAvailable(lci)
    set(RECONVERSE_ENABLE_COMM_LCI2 ON)
  endif()

  if(RECONVERSE_ENABLE_COMM_LCI2)
    message(STATUS "LCIv2 found. Enabling LCIv2 backend.")
    target_link_libraries(reconverse PRIVATE LCI::lci)
    target_sources(reconverse PRIVATE lci2/comm_backend_lci2.cpp)
    set(RECONVERSE_ENABLE_COMM_LCI2 ON PARENT_SCOPE)
  else()
    message(
      WARNING
        "LCI2 is not found. The LCI2 backend is not enabled. Try setting RECONVERSE_AUTOFETCH_LCI2=ON to fetch LCIv2 automatically."
    )
  endif()
endif()

# LCI1
if(NOT RECONVERSE_ENABLE_COMM_LCI2 AND RECONVERSE_TRY_ENABLE_COMM_LCI1)
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
    message(STATUS "LCIv1 found. Enabling LCIv1 backend.")
    message(WARNING "LCIv1 is deprecated. Please use LCIv2 instead.")
    target_link_libraries(reconverse PRIVATE LCI::LCI)
    target_sources(reconverse PRIVATE lci1/comm_backend_lci1.cpp)
    set(RECONVERSE_ENABLE_COMM_LCI1 ON PARENT_SCOPE)
  else()
    message(
      WARNING
        "LCIv1 is not found. The LCIv1 backend is not enabled. Try setting RECONVERSE_AUTOFETCH_LCI1=ON to fetch LCIv1 automatically."
    )
  endif()
endif()
