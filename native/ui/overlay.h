#pragma once
#include <cstdint>

// Dear ImGui overlay over the Vulkan backend: the real FPS counter the
// settings toggle and, later, the options menu itself. The Vulkan types stay
// behind void* so plain host code can include this freely.
//
// The counter runs on the render thread: ImGui is fed the display size and
// delta time directly, with no SDL3 platform backend. SDL's Android event
// loop owns the main thread, and the ImGui SDL3 backend is not built to be
// driven from another one (the milestone 3-a field test showed a blank
// overlay with it, ressalva R6).
namespace forza::ui {

// the forza.toml fps_counter setting; the options table in game_info.cpp owns it
extern bool g_fps_counter;

// ImGui context only: no platform backend behind it
void InitWindow();

// Vulkan-side ImGui backend, once the device exists
struct VulkanInit {
    void* instance;
    void* physical_device;
    void* device;
    uint32_t queue_family;
    void* queue;
    void* render_pass;  // the overlay pass the backend records the UI in
    uint32_t image_count;
};
bool InitVulkan(const VulkanInit& info);

// once per presented frame: keeps the FPS measurement and the frame time current
void PresentTick();

// inside an open overlay render pass: NewFrame, the FPS counter and the draw
// data. w and h are the target the pass renders into, in pixels.
void Record(void* command_buffer, float w, float h);

void Shutdown();

}  // namespace forza::ui
