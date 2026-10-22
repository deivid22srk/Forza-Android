# The graphics backends: Vulkan (vulkan.cpp, milestone 3) behind the factory in
# backend.cpp, with the no-op backend as the fallback when Vulkan cannot start.
forza_add_host_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan.cpp)
target_include_directories(main PRIVATE
    ${SDK}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends)

# The graphics layer compiles the SDK's generated HLSL with shaderc at runtime
# (the D3D12 reference compiles it with D3DCompile, this side uses the glslang
# front end inside the game's process). Google stopped bundling shaderc with
# the NDK (verified absent from the r27b/r27c and r28.2 packages), so the CI
# builds it from the pinned revision via ci/build-shaderc.sh and points this
# build at the result through FORZA_SHADERC_DIR. Local builds with an NDK that
# still bundles it need nothing. The build dies here when neither source is
# available, so a broken toolchain is loud instead of a game that draws
# nothing.
if(NOT ANDROID)
    return()
endif()
set(FORZA_SHADERC "")
if(FORZA_SHADERC_DIR AND EXISTS ${FORZA_SHADERC_DIR}/include/shaderc/shaderc.h)
    set(FORZA_SHADERC ${FORZA_SHADERC_DIR})
elseif(DEFINED ENV{FORZA_SHADERC_DIR}
        AND EXISTS $ENV{FORZA_SHADERC_DIR}/include/shaderc/shaderc.h)
    set(FORZA_SHADERC $ENV{FORZA_SHADERC_DIR})
elseif(EXISTS ${ANDROID_NDK}/sources/third_party/shaderc/include/shaderc/shaderc.h)
    set(FORZA_SHADERC ${ANDROID_NDK}/sources/third_party/shaderc)
endif()
if(NOT FORZA_SHADERC)
    message(FATAL_ERROR
        "shaderc not found; set FORZA_SHADERC_DIR to a directory holding "
        "include/shaderc/shaderc.h and libs/c++_static/<abi>/libshaderc.a "
        "(the CI builds it with ci/build-shaderc.sh from ci/shaderc-revision.txt). "
        "The game compiles its shaders with it at runtime.")
endif()
if(NOT EXISTS ${FORZA_SHADERC}/libs/c++_static/${ANDROID_ABI}/libshaderc.a)
    message(FATAL_ERROR
        "libshaderc.a for ${ANDROID_ABI} is missing under "
        "${FORZA_SHADERC}/libs/c++_static. This shaderc build does not carry "
        "a prebuilt static library for the target ABI.")
endif()
message(STATUS "shaderc: ${FORZA_SHADERC}")
target_include_directories(main PRIVATE ${FORZA_SHADERC}/include)
target_link_libraries(main PRIVATE ${FORZA_SHADERC}/libs/c++_static/${ANDROID_ABI}/libshaderc.a)
