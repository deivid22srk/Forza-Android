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
