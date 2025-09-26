if(RECONVERSE_TRY_ENABLE_COMM_LCW)
  # Try to find LCW externally
  find_package(
    lcw
    CONFIG
    HINTS
    ${LCI_ROOT}
    $ENV{LCI_ROOT}
    PATH_SUFFIXES
    lib/cmake
    lib64/cmake)
  if(NOT LCW_FOUND)
    message(STATUS "Existing LCW installation not found.")
  else()
    message(STATUS "Existing LCW installation found.")
    set(RECONVERSE_ENABLE_COMM_LCW ON)
  endif()

  if(NOT RECONVERSE_ENABLE_COMM_LCW AND RECONVERSE_AUTOFETCH_LCW)
    if(FETCHCONTENT_SOURCE_DIR_LCW)
      message(
        STATUS
          "LCW will be include through FetchContent from ${FETCHCONTENT_SOURCE_DIR_LCW}"
      )
    else()
      message(
        STATUS
          "LCW will be include throught FetchContent from GIT_TAG ${RECONVERSE_AUTOFETCH_LCW_TAG}"
      )
    endif()
    include(FetchContent)
    FetchContent_Declare(
      lcw
      GIT_REPOSITORY https://github.com/jiakunyan/lcw.git
      GIT_TAG ${RECONVERSE_AUTOFETCH_LCW_TAG})
    FetchContent_MakeAvailable(lcw)
    set(RECONVERSE_ENABLE_COMM_LCW ON)
  endif()

  if(RECONVERSE_ENABLE_COMM_LCW)
    message(STATUS "LCW found. Enabling LCW backend.")
    # LCW is a private dependency of reconverse
    # but we need to link it publicly in case reconverse is built as a static library
    target_link_libraries(reconverse PUBLIC LCW::lcw)
    target_sources(reconverse PRIVATE comm_backend_lcw.cpp)
    set(RECONVERSE_ENABLE_COMM_LCW ON PARENT_SCOPE)
  else()
    message(
      WARNING
        "LCW is not found. The LCW backend is not enabled. Try setting RECONVERSE_AUTOFETCH_LCW=ON to fetch LCW automatically."
    )
  endif()
endif()