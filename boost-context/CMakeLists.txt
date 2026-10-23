set(ABI sysv)
set(SYNTAX gas)
set(EXTENSION S)
if(CHARM_OS STREQUAL "darwin")
  set(FORMAT macho)
elseif(CHARM_OS STREQUAL "win")
  set(ABI ms)
  set(FORMAT pe)
  # MSVC cannot assemble the GAS sources; use Boost's MASM ones (ml64).
  # Charm++ sets CMK_COMPILER; a standalone build only has MSVC.
  if(MSVC OR CMK_COMPILER STREQUAL "msvc")
    set(SYNTAX masm)
    set(EXTENSION asm)
  endif()
else()
  set(FORMAT elf)
endif()
if(CHARM_CPU STREQUAL "arm7")
  set(ARCHITECTURE arm)
  set(ABI aapcs)
elseif(CHARM_CPU STREQUAL "arm8")
  set(ARCHITECTURE arm64)
  set(ABI aapcs)
elseif(CHARM_CPU STREQUAL "ppc64le")
  set(ARCHITECTURE ppc64)
elseif(CHARM_CPU STREQUAL "x86_64")
  set(ARCHITECTURE x86_64)
else()
  set(ARCHITECTURE i386)
endif()

# A native Windows build (MSVC set) assembles them with ml64 directly. Charm++
# on Windows instead drives MSVC from Cygwin through its unix2nt_cc wrapper,
# which hands .asm sources to ml64 itself, so there they go through the
# generic ASM language with that wrapper as the assembler.
if(SYNTAX STREQUAL "masm" AND MSVC)
  enable_language(C ASM_MASM)
else()
  enable_language(C ASM)
endif()

set(ufcontext_sources
    jump_${ARCHITECTURE}_${ABI}_${FORMAT}_${SYNTAX}.${EXTENSION}
    make_${ARCHITECTURE}_${ABI}_${FORMAT}_${SYNTAX}.${EXTENSION}
    ontop_${ARCHITECTURE}_${ABI}_${FORMAT}_${SYNTAX}.${EXTENSION}
)

add_library(boost-context OBJECT ${ufcontext_sources})
set_target_properties(boost-context PROPERTIES LINKER_LANGUAGE CXX)

if(SYNTAX STREQUAL "masm")
    # The MASM sources declare their PROCs with an EXPORT attribute only when
    # BOOST_CONTEXT_EXPORT is defined; it must be set (to nothing) for ml64.
    target_compile_definitions(boost-context PRIVATE BOOST_CONTEXT_EXPORT=)
endif()
