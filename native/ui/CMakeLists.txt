# Dear ImGui overlay: the real FPS counter now, the options menu on the same
# rails later. The core and the Vulkan backend are built into main; the SDL3
# platform backend is not: the counter runs on the render thread and SDL's
# Android event loop owns the main thread (ressalva R6).
forza_add_host_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(main PRIVATE
    ${SDK}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends)
