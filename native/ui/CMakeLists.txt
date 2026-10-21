# Dear ImGui overlay: the real FPS counter now, the options menu on the same
# rails later. The core and the Vulkan/SDL3 backends are built into main.
forza_add_host_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp)
target_include_directories(main PRIVATE
    ${SDK}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends)
