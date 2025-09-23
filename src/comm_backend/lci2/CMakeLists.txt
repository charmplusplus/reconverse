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
    set(LCI_VERSION ${LCI_VERSION} PARENT_SCOPE)
    set(RECONVERSE_ENABLE_COMM_LCI2 ON)
  endif()

  if(RECONVERSE_ENABLE_COMM_LCI2)
    message(STATUS "LCIv2 found. Enabling LCIv2 backend.")
    # LCI is a private dependency of reconverse
    # but we need to link it publicly in case reconverse is built as a static library
    target_link_libraries(reconverse PUBLIC LCI::lci)
    target_sources(reconverse PRIVATE comm_backend_lci2.cpp)
    set(RECONVERSE_ENABLE_COMM_LCI2 ON PARENT_SCOPE)
  else()
    message(
      WARNING
        "LCI2 is not found. The LCI2 backend is not enabled. Try setting RECONVERSE_AUTOFETCH_LCI2=ON to fetch LCIv2 automatically."
    )
  endif()
endif()
