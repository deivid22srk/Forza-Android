// Dear ImGui overlay: the real FPS counter for milestone 3 (the options menu
// comes later on the same rails). ImGui draws after the frame's blit, inside
// the backend's overlay render pass, so the counter is on screen even while
// the game itself still draws nothing. The window takes no input: touches
// reach the game through it.
#include "overlay.h"

#include <chrono>
#include <cstdint>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "core/log.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

namespace forza::ui {

bool g_fps_counter = true;

namespace {

bool g_window_ok = false, g_vulkan_ok = false;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
uint64_t g_frames = 0, g_window_frames = 0;
std::chrono::steady_clock::time_point g_window_start = std::chrono::steady_clock::now();
float g_fps = 0.0f;

void CheckVkResult(VkResult err) {
    if (err != VK_SUCCESS) LOGE("ui: ImGui Vulkan error %d", int(err));
}

}  // namespace

void InitWindow(SDL_Window* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini beside the game files
    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        LOGE("ui: ImGui SDL3 init failed");
        ImGui::DestroyContext();
        return;
    }
    g_window_ok = true;
}

bool InitVulkan(const VulkanInit& info) {
    if (!g_window_ok || !info.device || !info.render_pass) return false;
    g_device = (VkDevice)info.device;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 8;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g_device, &pool, nullptr, &g_pool) != VK_SUCCESS) {
        LOGE("ui: ImGui descriptor pool failed");
        return false;
    }
    ImGui_ImplVulkan_InitInfo v{};
    v.Instance = (VkInstance)info.instance;
    v.PhysicalDevice = (VkPhysicalDevice)info.physical_device;
    v.Device = g_device;
    v.QueueFamily = info.queue_family;
    v.Queue = (VkQueue)info.queue;
    v.DescriptorPool = g_pool;
    v.RenderPass = (VkRenderPass)info.render_pass;
    v.MinImageCount = 2;
    v.ImageCount = info.image_count;
    v.CheckVkResultFn = CheckVkResult;
    if (!ImGui_ImplVulkan_Init(&v)) {
        LOGE("ui: ImGui Vulkan init failed");
        vkDestroyDescriptorPool(g_device, g_pool, nullptr);
        g_pool = VK_NULL_HANDLE;
        return false;
    }
    if (!ImGui_ImplVulkan_CreateFontsTexture()) LOGW("ui: ImGui font upload failed, the counter may not draw");
    g_vulkan_ok = true;
    return true;
}

void PresentTick() {
    g_frames++;
    g_window_frames++;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g_window_start).count();
    if (secs >= 1) {
        g_fps = float(double(g_window_frames) / secs);
        g_window_frames = 0;
        g_window_start = now;
    }
}

void Record(void* command_buffer) {
    if (!g_window_ok || !g_vulkan_ok || !g_fps_counter) return;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    if (ImGui::Begin("Forza", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBackground)) {
        ImGui::Text("FPS %.1f (%llu)", g_fps, (unsigned long long)g_frames);
    }
    ImGui::End();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)command_buffer);
}

void Shutdown() {
    if (!g_window_ok) return;
    if (g_vulkan_ok) ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (g_device && g_pool) vkDestroyDescriptorPool(g_device, g_pool, nullptr);
    g_pool = VK_NULL_HANDLE;
    g_vulkan_ok = g_window_ok = false;
}

}  // namespace forza::ui
