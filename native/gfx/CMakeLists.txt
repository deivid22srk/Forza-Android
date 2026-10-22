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

# The NDK ships libshaderc prebuilt for runtime HLSL to SPIR-V (the SDK's
# gpu/shaders.cpp emits HLSL per draw; the D3D12 reference compiles it with
# D3DCompile, this side compiles it with the glslang front end inside the
# game's process). The build dies here when the NDK install lacks it, so a
# broken toolchain is loud instead of a game that draws nothing.
if(NOT ANDROID)
    return()
endif()
set(FORZA_SHADERC ${ANDROID_NDK}/sources/third_party/shaderc)
if(NOT EXISTS ${FORZA_SHADERC}/include/shaderc/shaderc.h)
    message(FATAL_ERROR
        "shaderc is missing from the NDK at ${ANDROID_NDK} "
        "(sources/third_party/shaderc/include/shaderc/shaderc.h not found). "
        "Milestone 3-b compiles the game's shaders with it at runtime.")
endif()
set(FORZA_SHADERC_LIB "")
foreach(v c++_shared c++_static)
    if(NOT FORZA_SHADERC_LIB AND EXISTS ${FORZA_SHADERC}/libs/${v}/${ANDROID_ABI}/libshaderc.a)
        set(FORZA_SHADERC_LIB ${FORZA_SHADERC}/libs/${v}/${ANDROID_ABI}/libshaderc.a)
    endif()
endforeach()
if(NOT FORZA_SHADERC_LIB)
    message(FATAL_ERROR
        "libshaderc.a for ${ANDROID_ABI} is missing from ${FORZA_SHADERC}/libs. "
        "This NDK snapshot carries no prebuilt shaderc for the target ABI.")
endif()
message(STATUS "shaderc (NDK): ${FORZA_SHADERC_LIB}")
target_include_directories(main PRIVATE ${FORZA_SHADERC}/include)
target_link_libraries(main PRIVATE ${FORZA_SHADERC_LIB})
