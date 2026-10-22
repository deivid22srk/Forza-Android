// Dear ImGui overlay: the real FPS counter for milestone 3 (the options menu
// comes later on the same rails). ImGui draws after the frame's blit, inside
// the backend's overlay render pass, so the counter is on screen even while
// the game itself still draws nothing.
//
// No SDL3 platform backend: the record runs on the render thread and SDL's
// Android event loop owns the main thread (ressalva R6, confirmed in the
// 3-a field test where the counter never showed). The counter needs no input,
// so the display size comes in from the backend and the delta time is
// measured between presents.
#include "overlay.h"

#include <chrono>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "core/log.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"

namespace forza::ui {

bool g_fps_counter = true;

namespace {

bool g_vulkan_ok = false;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
uint64_t g_frames = 0, g_window_frames = 0;
std::chrono::steady_clock::time_point g_window_start = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_last_frame = g_window_start;
float g_fps = 0.0f;
float g_delta = 1.0f / 30.0f;

void CheckVkResult(VkResult err) {
    if (err != VK_SUCCESS) LOGE("ui: ImGui Vulkan error %d", int(err));
}

}  // namespace

void InitWindow() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini beside the game files
    // the 13 px default atlas vanishes on a 2400x1080 phone screen at arm's
    // length; the counter is the one thing that has to stay readable (field
    // report 3-b round 1: black screen, no counter in sight)
    ImFontConfig cfg{};
    cfg.SizePixels = 40.0f;
    io.Fonts->AddFontDefault(&cfg);
}

bool InitVulkan(const VulkanInit& info) {
    if (!info.device || !info.render_pass) return false;
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
    LOGI("ui: ImGui overlay ready on the swapchain render pass");
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
    float dt = float(std::chrono::duration<double>(now - g_last_frame).count());
    g_last_frame = now;
    // a backgrounded app can come back after any pause; keep ImGui's timers sane
    g_delta = dt > 0.0001f && dt < 1.0f ? dt : 1.0f / 30.0f;
}

void Record(void* command_buffer, float w, float h) {
    if (!g_vulkan_ok || !g_fps_counter) return;
    LOG_FIRST(1, "ui: the overlay records, display %.0fx%.0f", w, h);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = g_delta;
    ImGui_ImplVulkan_NewFrame();
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
    ImDrawData* dd = ImGui::GetDrawData();
    LOG_FIRST(1, "ui: overlay draw data: %d vertices, %d indices", dd ? dd->TotalVtxCount : -1,
              dd ? dd->TotalIdxCount : -1);
    ImGui_ImplVulkan_RenderDrawData(dd, (VkCommandBuffer)command_buffer);
}

void Shutdown() {
    if (!g_vulkan_ok) {
        ImGui::DestroyContext();
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    if (g_device && g_pool) vkDestroyDescriptorPool(g_device, g_pool, nullptr);
    g_pool = VK_NULL_HANDLE;
    g_vulkan_ok = false;
}

}  // namespace forza::ui
