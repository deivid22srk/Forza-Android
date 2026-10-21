# The portable XboxXBE-Recomp runtime plus the recompiled game code.
#
# Dropped Windows-only pieces and the parts the Android host replaces:
#   runtime/app/*                 -> native/platform
#   runtime/core/input.cpp        -> native/platform/input.cpp
#   runtime/core/log.cpp          -> native/core/log.cpp (logcat needs info lines in release)
#   runtime/gpu/d3d12.cpp         -> native/gfx (null backend now, Vulkan later)
#   runtime/gpu/render_thread.cpp -> native/gfx/backend.cpp
#   runtime/audio/xaudio2.cpp     -> native/audio/output.cpp
#   runtime/xdk/xnet.cpp          -> Windows sockets; milestone 2+ (POSIX or disabled)

if(NOT EXISTS ${FORZA_GENERATED_DIR}/x86_func_mapping.cpp)
    message(FATAL_ERROR
        "No recompiled code in ${FORZA_GENERATED_DIR}. Run the CI build (or tools/xbe_recomp) first, "
        "or configure with -DFORZA_STUB=ON.")
endif()
if(NOT EXISTS ${FORZA_XBE})
    message(FATAL_ERROR "The XBE this build is made from is missing: ${FORZA_XBE}")
endif()

file(GLOB SDK_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${SDK}/runtime/core/*.cpp
    ${SDK}/runtime/cpu/*.cpp
    ${SDK}/runtime/kernel/*.cpp
    ${SDK}/runtime/gpu/*.cpp
    ${SDK}/runtime/xdk/*.cpp)
list(FILTER SDK_RUNTIME_SOURCES EXCLUDE REGEX "/runtime/core/input\\.cpp$")
list(FILTER SDK_RUNTIME_SOURCES EXCLUDE REGEX "/runtime/core/log\\.cpp$")
list(FILTER SDK_RUNTIME_SOURCES EXCLUDE REGEX "/runtime/gpu/(d3d12|render_thread)\\.cpp$")
list(FILTER SDK_RUNTIME_SOURCES EXCLUDE REGEX "/runtime/xdk/xnet\\.cpp$")
forza_add_host_sources(${SDK_RUNTIME_SOURCES})

# Kernel exports the SDK still lacks. Registered on top of the SDK runtime, so
# they must stay out of ${SDK}; see the file comment and docs/GAPS.md.
forza_add_host_sources(${CMAKE_CURRENT_SOURCE_DIR}/overrides.cpp)

# The partition count is what xbe_symbols wrote. Keep in sync with config/game.toml.
file(STRINGS ${FORZA_CONFIG} _partitions REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${_partitions}")
if(NOT GAME_PARTITIONS)
    message(FATAL_ERROR "${FORZA_CONFIG} has no partitions = N line; run xbe_symbols first")
endif()
math(EXPR _last_partition "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES ${FORZA_GENERATED_DIR}/x86_func_mapping.cpp)
foreach(_i RANGE ${_last_partition})
    list(APPEND GEN_SOURCES ${FORZA_GENERATED_DIR}/x86_recomp.${_i}.cpp)
endforeach()
foreach(_src IN LISTS GEN_SOURCES)
    if(NOT EXISTS ${_src})
        message(FATAL_ERROR "Missing recompiler output ${_src}; run xbe_recomp again")
    endif()
endforeach()
forza_add_generated_sources(${GEN_SOURCES})

# The SHA-1 of the XBE this build was made from: app::FindGame refuses another release.
file(SHA1 ${FORZA_XBE} FORZA_XBE_SHA1)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp
     CONTENT "extern const char kImageSha1[] = \"${FORZA_XBE_SHA1}\";\n")
forza_add_host_sources(${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp)

target_include_directories(main PRIVATE
    ${SDK}/runtime
    ${SDK}/harness/compat
    ${FORZA_GENERATED_DIR}
    ${CMAKE_BINARY_DIR})
