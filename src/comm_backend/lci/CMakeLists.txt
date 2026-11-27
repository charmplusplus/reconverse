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
    if(RECONVERSE_ENABLE_CUDA)
      set(LCI_USE_CUDA ON CACHE BOOL "Enable CUDA support in LCI" FORCE)
    endif()
    if(RECONVERSE_ENABLE_HIP)
      set(LCI_USE_HIP ON CACHE BOOL "Enable HIP support in LCI" FORCE)
    endif()
    # Compile in LCI's registration cache (LCI cannot build it on Apple).
    # At run time it is on for ibv, where repeated sends of long-lived buffers
    # hit it instead of re-registering, and off on OFI, where libfabric caches
    # registrations itself (see CommBackendLCI::init).
    # LCI_ATTR_USE_REG_CACHE overrides either default.
    if(NOT APPLE)
      set(LCI_USE_REG_CACHE ON CACHE BOOL
          "Enable LCI's memory registration cache" FORCE)
    endif()
    FetchContent_Declare(
      lci
      GIT_REPOSITORY https://github.com/uiuc-hpc/lci.git
      GIT_TAG ${RECONVERSE_AUTOFETCH_LCI2_TAG})
    FetchContent_MakeAvailable(lci)
    # LCI vendors a UCX snapshot whose ucs/sys/stubs.h declares
    #   void ucs_empty_function();
    # with an empty parameter list. Through C17 that means "unspecified
    # parameters", so ucs/async/async.c is free to initialise typed function
    # pointers with it. C23 redefines () as (void), which makes those
    # initialisers incompatible-pointer errors -- and GCC 15 compiles as
    # -std=gnu23 by default. That is the whole reason this snapshot builds
    # under GCC 14 and breaks under GCC 15 (charmplusplus/reconverse#247);
    # it is not an arm, CUDA or optimisation-level effect, though #247 was
    # first seen through an arm CUDA build.
    # The snapshot is pre-C23 C, so pin it to the standard it was written
    # against rather than silencing the diagnostic: -Wno-incompatible-pointer-types
    # would hide real mismatches in the same target, and patching the vendored
    # source would be overwritten by the next LCI bump.
    if(TARGET lci-ucx)
      set_property(TARGET lci-ucx PROPERTY C_STANDARD 17)
    endif()
    set(LCI_VERSION ${LCI_VERSION} PARENT_SCOPE)
    set(RECONVERSE_ENABLE_COMM_LCI2 ON)
  endif()

  if(RECONVERSE_ENABLE_COMM_LCI2)
    message(STATUS "LCIv2 found. Enabling LCIv2 backend.")
    # LCI is a private dependency of reconverse
    # but we need to link it publicly in case reconverse is built as a static library.
    # LCI::lct too: comm_backend_lci.cpp calls LCT functions (LCT_pmi_abort), and an
    # installed LCI's LCI::lci target does not link liblct.
    target_link_libraries(reconverse PUBLIC LCI::lci LCI::lct)
    target_sources(reconverse PRIVATE comm_backend_lci.cpp)
    # AllocatorLCI gives LCI the message pool's registration for a rendezvous
    # AM's receive buffer through allocator_base_t::allocate_with_mr, which LCI
    # added in 6f0cda12 (uiuc-hpc/lci#226). An installed LCI may be older, and
    # LCI has no version or macro that says so, so try compiling an override.
    # Without the hook the override is left out, and LCI registers each such
    # buffer itself, as before. A fetched LCI is the pinned tag, which has it;
    # its target is not built yet, so it could not be tried anyway.
    get_target_property(_reconverse_lci_imported LCI::lci IMPORTED)
    if(_reconverse_lci_imported)
      include(CheckCXXSourceCompiles)
      set(CMAKE_REQUIRED_LIBRARIES LCI::lci LCI::lct)
      check_cxx_source_compiles("
        #include \"lci.hpp\"
        struct A : lci::allocator_base_t {
          void *allocate(size_t) override { return nullptr; }
          void deallocate(void *) override {}
          void *allocate_with_mr(size_t, lci::device_t, lci::mr_t *) override {
            return nullptr;
          }
        };
        int main() { A a; return a.allocate(0) != nullptr; }"
        RECONVERSE_LCI_ALLOCATE_WITH_MR)
      unset(CMAKE_REQUIRED_LIBRARIES)
      if(NOT RECONVERSE_LCI_ALLOCATE_WITH_MR)
        message(
          STATUS
            "The installed LCI has no allocator_base_t::allocate_with_mr (LCI 6f0cda12 and later): LCI registers every rendezvous AM receive buffer itself"
        )
      endif()
    else()
      set(RECONVERSE_LCI_ALLOCATE_WITH_MR ON)
    endif()
    if(RECONVERSE_LCI_ALLOCATE_WITH_MR)
      target_compile_definitions(reconverse PRIVATE RECONVERSE_LCI_ALLOCATE_WITH_MR=1)
    endif()
    set(RECONVERSE_ENABLE_COMM_LCI2 ON PARENT_SCOPE)
  else()
    message(
      WARNING
        "LCIv2 is not found. The LCI backend is not enabled. Try setting RECONVERSE_AUTOFETCH_LCI2=ON to fetch LCIv2 automatically."
    )
  endif()
endif()
