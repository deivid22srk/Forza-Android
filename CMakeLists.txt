# Forza Motorsport (original Xbox), statically recompiled, as Android arm64:
# libmain.so, loaded by SDL's activity.
#
# The game's x86 code is recompiled in CI (tools/ + xbe_symbols + xbe_recomp)
# into generated/game, then compiled here with the NDK together with the SDK's
# portable runtime and the Android host in native/.

cmake_minimum_required(VERSION 3.22)
project(forza_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FORZA_STUB "Build the SDL-only stub APK (no game code)" OFF)
option(FORZA_LTO "Enable ThinLTO for the Android target" OFF)

set(FORZA_SDK_DIR "" CACHE PATH "XboxXBE-Recomp checkout (defaults to sdk/)")
set(FORZA_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/game" CACHE PATH "xbe_recomp output")
set(FORZA_XBE "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe" CACHE FILEPATH "the XBE the build was made from")
set(FORZA_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml" CACHE FILEPATH "recompiler config")

set(FORZA_REPO ${CMAKE_CURRENT_SOURCE_DIR})
set(FORZA_NATIVE ${FORZA_REPO}/native)
set(FORZA_COMPAT_DIR ${FORZA_NATIVE}/platform/compat)

# The build's version in the log: the commit when the project is in git.
set(FORZA_COMMIT "dev")
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    WORKING_DIRECTORY ${FORZA_REPO}
                    OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _failed)
    if(NOT _failed AND _head)
        set(FORZA_COMMIT ${_head})
    endif()
endif()

include(${FORZA_REPO}/cmake/Forza.cmake)

# ---------------------------------------------------------------------------
# SDK
# ---------------------------------------------------------------------------
if(FORZA_SDK_DIR AND EXISTS ${FORZA_SDK_DIR}/cmake/XbeRecomp.cmake)
    set(SDK ${FORZA_SDK_DIR})
elseif(EXISTS ${FORZA_REPO}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${FORZA_REPO}/sdk)
elseif(EXISTS ${FORZA_REPO}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    get_filename_component(SDK ${FORZA_REPO}/../XboxXBE-Recomp ABSOLUTE)
else()
    # Pinned so a build is reproducible; CI checks out ci/sdk-revision.txt.
    include(FetchContent)
    FetchContent_Declare(xbe_sdk
        GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git
        GIT_TAG 1bb872b181c40de83ca2f69d9cd45b442a362d1d
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
message(STATUS "XboxXBE-Recomp SDK: ${SDK}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# SDL3 comes from third_party/SDL, cloned by Gradle's fetchSDL task or by CI.
set(SDL_DIR ${FORZA_REPO}/third_party/SDL)
if(NOT EXISTS ${SDL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
        "SDL3 is missing from ${SDL_DIR}. The Gradle build clones it (fetchSDL). "
        "For a manual configure, clone https://github.com/libsdl-org/SDL at release-3.4.16 into third_party/SDL.")
endif()
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

if(NOT FORZA_STUB)
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus)
endif()

# ---------------------------------------------------------------------------
# libmain.so
# ---------------------------------------------------------------------------
add_library(main SHARED)

if(FORZA_STUB)
    target_sources(main PRIVATE ${FORZA_NATIVE}/platform/stub_main.cpp)
    target_include_directories(main PRIVATE ${FORZA_COMPAT_DIR})
    target_link_libraries(main PRIVATE SDL3::SDL3 android log)
else()
    add_subdirectory(native)
    target_include_directories(main PRIVATE
        ${FORZA_NATIVE}
        ${FORZA_NATIVE}/platform
        ${FORZA_NATIVE}/platform/compat
        ${SDK}/runtime
        ${SDK}/harness/compat
        ${FORZA_GENERATED_DIR}
        ${CMAKE_BINARY_DIR})
    target_link_libraries(main PRIVATE
        SDL3::SDL3
        tomlplusplus::tomlplusplus
        android
        log)
endif()

# ---------------------------------------------------------------------------
# Flags: arm64 only, armv8-a baseline, low-cost phone tuning.
# ---------------------------------------------------------------------------
target_compile_options(main PRIVATE
    -fno-strict-aliasing
    -fsigned-char
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden)
target_link_options(main PRIVATE
    -Wl,--gc-sections
    -Wl,-z,max-page-size=16384)

if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(main PRIVATE -march=armv8-a -mtune=cortex-a55)
endif()

if(FORZA_LTO)
    target_compile_options(main PRIVATE -flto=thin)
    target_link_options(main PRIVATE -flto=thin)
endif()

target_compile_definitions(main PRIVATE FORZA_COMMIT="${FORZA_COMMIT}")
