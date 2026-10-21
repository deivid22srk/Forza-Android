# Shared CMake helpers for the Forza Android host.

# Adds host sources to the single libmain target and force-includes the bionic
# compatibility header (memfd_create on API 29, ftruncate visibility).
function(forza_add_host_sources)
    target_sources(main PRIVATE ${ARGN})
    foreach(_src IN LISTS ARGN)
        # The calls come from native/<module> subdirectories, but main is made in
        # the top level: the source property must be set in that directory's
        # scope, or the compiler never sees it.
        set_property(SOURCE ${_src} TARGET_DIRECTORY main APPEND PROPERTY COMPILE_OPTIONS
            "-include;${FORZA_COMPAT_DIR}/crt.h")
    endforeach()
endfunction()

# Adds recompiler output. It is generated code: no project warnings, x86
# semantics (signed char, no strict aliasing) and a fixed optimisation level.
function(forza_add_generated_sources)
    target_sources(main PRIVATE ${ARGN})
    foreach(_src IN LISTS ARGN)
        set_property(SOURCE ${_src} TARGET_DIRECTORY main APPEND PROPERTY COMPILE_OPTIONS
            "-O2;-w;-fno-strict-aliasing;-fsigned-char")
    endforeach()
endfunction()
