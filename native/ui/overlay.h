#pragma once
#include <cstdint>

struct SDL_Window;

// Dear ImGui overlay over the Vulkan backend: the real FPS counter the
// settings toggle and, later, the options menu itself. The Vulkan types stay
// behind void* so plain host code can include this freely.
namespace forza::ui {

// the forza.toml fps_counter setting; the options table in game_info.cpp owns it
extern bool g_fps_counter;

// ImGui context and SDL3 backend, once the window exists
void InitWindow(SDL_Window* window);

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

// once per presented frame: keeps the FPS measurement current
void PresentTick();

// inside an open overlay render pass: NewFrame, the FPS counter and the draw data
void Record(void* command_buffer);

void Shutdown();

}  // namespace forza::ui
