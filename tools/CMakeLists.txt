# The host-side recompiler tools, built on Linux (CI does this on Ubuntu).
# The Android build itself only consumes their output.
cmake_minimum_required(VERSION 3.22)
project(forza_tools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(FORZA_REPO ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

set(SDK_DIR "" CACHE PATH "XboxXBE-Recomp checkout")
if(NOT SDK_DIR)
    if(EXISTS ${FORZA_REPO}/sdk/cmake/XbeRecomp.cmake)
        set(SDK_DIR ${FORZA_REPO}/sdk)
    elseif(EXISTS ${FORZA_REPO}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
        get_filename_component(SDK_DIR ${FORZA_REPO}/../XboxXBE-Recomp ABSOLUTE)
    else()
        message(FATAL_ERROR "Set -DSDK_DIR=<XboxXBE-Recomp checkout> (CI clones it from ci/sdk-revision.txt)")
    endif()
endif()
message(STATUS "XboxXBE-Recomp SDK: ${SDK_DIR}")

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG v4.1.1
    GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus)

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${SDK_DIR}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${SDK_DIR}/runtime ${SDK_DIR}/tools/recomp)
target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(xbe_recomp PRIVATE -fno-strict-aliasing)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

add_executable(xbe_symbols ${SDK_DIR}/tools/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${SDK_DIR}/runtime)
target_compile_definitions(xbe_symbols PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(xbe_symbols PRIVATE -fno-strict-aliasing)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)
