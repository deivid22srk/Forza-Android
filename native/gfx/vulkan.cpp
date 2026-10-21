// Milestone 3-a Vulkan renderer. The D3D8 HLE leaves the frame in the guest's
// own surfaces: Clear and Copy write them on the CPU (the CPU is the only
// writer in this milestone), and Present decodes the front surface with the
// SDK's own decoder (gpu/texture.cpp), applies the gamma ramp and blits it to
// the swapchain with the guest aspect kept. The Dear ImGui FPS counter records
// on top in the same submit. Draw reports itself and waits for the shader
// translation (3-b): menus render once that lands.
#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "host.h"
#include "kernel/kernel.h"
#include "ui/overlay.h"
#include "vulkan.h"

namespace gpu {

namespace {

constexpr uint32_t kFramesInFlight = 2;

#define FORZA_VK(f)                                     \
    do {                                                \
        if (VkResult r = (f); r != VK_SUCCESS) {        \
            LOGE("gpu: %s failed with %d", #f, int(r)); \
            return false;                               \
        }                                               \
    } while (0)

// a failure inside a frame: report once per site and drop the frame, not the app
#define FORZA_VK_SOFT(f, ...)                    \
    do {                                         \
        if (VkResult r = (f); r != VK_SUCCESS) { \
            LOG_FIRST(8, __VA_ARGS__, int(r));   \
            return false;                        \
        }                                        \
    } while (0)

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t wanted, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((wanted & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

// float to half, for the F16 depth clear
uint16_t Half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, mag = x & 0x7FFFFFFF;
    if (mag >= 0x47800000) return uint16_t(sign | 0x7C00);
    if (mag < 0x38800000) return uint16_t(sign);
    int e = int((mag >> 23) - 127) + 15, m = (mag >> 13) & 0x3FF;
    if (e <= 0) return uint16_t(sign | ((mag | 0x800000) >> (13 - e)));
    return uint16_t(sign | (e << 10) | m);
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;

    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
              uint32_t dst_points_va) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t* ramp) override;

private:
    void DestroySwapchain();
    bool CreateSurfaceAndSwapchain();
    bool EnsureGuestBuffers(uint32_t w, uint32_t h, uint32_t flight);
    void ApplyGamma(std::vector<uint8_t>& px) const;
    bool SubmitFrame(const std::vector<uint8_t>& px, uint32_t flight);

    bool ok_ = false, recreate_next_ = false;
    bool ui_ready_ = false;  // the overlay backend survives swapchain rebuilds
    uint32_t guest_w_ = 0, guest_h_ = 0;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    VkFormat swap_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{1920, 1080};
    VkRenderPass overlay_pass_ = VK_NULL_HANDLE;  // survives swapchain rebuilds, the format does not change
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> rendered_;  // one per swapchain image
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds_;  // one per swapchain image
    VkBuffer staging_[kFramesInFlight] = {};
    VkDeviceMemory staging_mem_[kFramesInFlight] = {};
    uint8_t* staging_map_[kFramesInFlight] = {};
    VkDeviceSize staging_size_[kFramesInFlight] = {};
    VkImage frame_[kFramesInFlight] = {};
    VkDeviceMemory frame_mem_[kFramesInFlight] = {};
    uint32_t frame_w_[kFramesInFlight] = {}, frame_h_[kFramesInFlight] = {};
    VkSemaphore acquired_[kFramesInFlight] = {};
    VkFence fences_[kFramesInFlight] = {};
    uint32_t flight_ = 0;
    uint32_t epoch_ = 0;
    uint8_t gamma_[3 * 256] = {};
    bool gamma_identity_ = true;
};

VulkanBackend::~VulkanBackend() {
    if (!device_) {
        if (instance_) {
            if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            vkDestroyInstance(instance_, nullptr);
        }
        return;
    }
    vkDeviceWaitIdle(device_);
    DestroySwapchain();
    if (overlay_pass_) vkDestroyRenderPass(device_, overlay_pass_, nullptr);
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (staging_[i]) vkDestroyBuffer(device_, staging_[i], nullptr);
        if (staging_mem_[i]) vkFreeMemory(device_, staging_mem_[i], nullptr);
        if (frame_[i]) vkDestroyImage(device_, frame_[i], nullptr);
        if (frame_mem_[i]) vkFreeMemory(device_, frame_mem_[i], nullptr);
    }
    for (VkSemaphore s : acquired_) vkDestroySemaphore(device_, s, nullptr);
    for (VkFence f : fences_) vkDestroyFence(device_, f, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    SDL_Window* window = (SDL_Window*)host::Window();
    if (!window) {
        LOGE("gpu: Vulkan needs the SDL window before the game boots");
        return false;
    }
    uint32_t n = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!exts || !n) {
        LOGE("gpu: no Vulkan instance extensions from SDL");
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Forza Motorsport";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    ic.enabledExtensionCount = n;
    ic.ppEnabledExtensionNames = exts;
    FORZA_VK(vkCreateInstance(&ic, nullptr, &instance_));
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        LOGE("gpu: Vulkan surface failed: %s", SDL_GetError());
        return false;
    }
    // a device with graphics and present in one family: frames are recorded and
    // shown from the same thread
    uint32_t ndev = 0;
    if (vkEnumeratePhysicalDevices(instance_, &ndev, nullptr) != VK_SUCCESS || !ndev) {
        LOGE("gpu: no Vulkan physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(ndev);
    FORZA_VK(vkEnumeratePhysicalDevices(instance_, &ndev, devs.data()));
    for (VkPhysicalDevice d : devs) {
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nfam, nullptr);
        std::vector<VkQueueFamilyProperties> fams(nfam);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nfam, fams.data());
        for (uint32_t i = 0; i < nfam; i++) {
            if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (!present) continue;
            physical_ = d;
            queue_family_ = i;
            break;
        }
        if (physical_) break;
    }
    if (!physical_) {
        LOGE("gpu: no Vulkan device with graphics+present");
        return false;
    }
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    float prio = 1.0f;
    qc.queueFamilyIndex = queue_family_;
    qc.queueCount = 1;
    qc.pQueuePriorities = &prio;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    dc.enabledExtensionCount = 1;
    dc.ppEnabledExtensionNames = dev_exts;
    FORZA_VK(vkCreateDevice(physical_, &dc, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = queue_family_;
    FORZA_VK(vkCreateCommandPool(device_, &pc, nullptr, &pool_));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        FORZA_VK(vkCreateSemaphore(device_, &sci, nullptr, &acquired_[i]));
        FORZA_VK(vkCreateFence(device_, &fci, nullptr, &fences_[i]));
    }
    if (!CreateSurfaceAndSwapchain()) return false;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    LOGR("gpu: Vulkan ready on %s: guest %ux%u, swapchain %ux%u", props.deviceName, guest_w_, guest_h_, extent_.width,
         extent_.height);
    return true;
}

// The swapchain side of the renderer, rebuilt whenever the Android surface is
// replaced (background/rotation): images, views, framebuffers, the per-image
// command buffers and present semaphores, and the overlay pass the first time.
bool VulkanBackend::CreateSurfaceAndSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    FORZA_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX || !extent_.width || !extent_.height) {
        // the window is not laid out yet: take the SDL size inside the caps
        int pw = 0, ph = 0;
        host::WindowPixels(pw, ph);
        extent_.width =
            std::clamp(uint32_t(std::max(pw, 1)), caps.minImageExtent.width, std::max(caps.maxImageExtent.width, 1u));
        extent_.height =
            std::clamp(uint32_t(std::max(ph, 1)), caps.minImageExtent.height, std::max(caps.maxImageExtent.height, 1u));
    }
    extent_.width = std::clamp(extent_.width, caps.minImageExtent.width, std::max(caps.maxImageExtent.width, 1u));
    extent_.height = std::clamp(extent_.height, caps.minImageExtent.height, std::max(caps.maxImageExtent.height, 1u));
    uint32_t nf = 0;
    FORZA_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &nf, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(nf);
    FORZA_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &nf, formats.data()));
    formats.resize(nf);
    // BGRA8 is what the SDK's decoder produces; RGBA8 differs by a swap the CPU
    // would have to make per present, so prefer it only if BGRA8 is missing
    VkSurfaceFormatKHR picked = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) picked = f;
    swap_format_ = picked.format;
    if (swap_format_ != VK_FORMAT_B8G8R8A8_UNORM && swap_format_ != VK_FORMAT_R8G8B8A8_UNORM) {
        LOGE("gpu: swapchain format %d is not an 8-bit colour format", int(swap_format_));
        return false;
    }
    uint32_t count = 3;
    if (caps.maxImageCount) count = std::clamp(count, caps.minImageCount, caps.maxImageCount);
    else
        count = std::max(count, caps.minImageCount);
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = count;
    sc.imageFormat = swap_format_;
    sc.imageColorSpace = picked.colorSpace;
    sc.imageExtent = extent_;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOGE("gpu: the swapchain does not take transfers, the blit cannot run");
        return false;
    }
    FORZA_VK(vkCreateSwapchainKHR(device_, &sc, nullptr, &swapchain_));
    uint32_t nimg = 0;
    FORZA_VK(vkGetSwapchainImagesKHR(device_, swapchain_, &nimg, nullptr));
    images_.resize(nimg);
    FORZA_VK(vkGetSwapchainImagesKHR(device_, swapchain_, &nimg, images_.data()));
    // the overlay pass, once: the swapchain format is stable on Android
    if (!overlay_pass_) {
        VkAttachmentDescription att{};
        att.format = swap_format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // the blit is already in the image
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        FORZA_VK(vkCreateRenderPass(device_, &rp, nullptr, &overlay_pass_));
    }
    views_.resize(nimg);
    framebuffers_.resize(nimg);
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = images_[i];
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = swap_format_;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        FORZA_VK(vkCreateImageView(device_, &vc, nullptr, &views_[i]));
        VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fc.renderPass = overlay_pass_;
        fc.attachmentCount = 1;
        fc.pAttachments = &views_[i];
        fc.width = extent_.width;
        fc.height = extent_.height;
        fc.layers = 1;
        FORZA_VK(vkCreateFramebuffer(device_, &fc, nullptr, &framebuffers_[i]));
    }
    cmds_.resize(nimg);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool_;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = nimg;
    FORZA_VK(vkAllocateCommandBuffers(device_, &ca, cmds_.data()));
    rendered_.resize(nimg);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < nimg; i++) FORZA_VK(vkCreateSemaphore(device_, &sci, nullptr, &rendered_[i]));
    // the acquire semaphores are redone with the swapchain: a rebuild abandons
    // whatever acquire was in the air, and a signalled-but-unconsumed semaphore
    // is undefined behaviour on the next take
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (acquired_[i]) vkDestroySemaphore(device_, acquired_[i], nullptr);
        FORZA_VK(vkCreateSemaphore(device_, &sci, nullptr, &acquired_[i]));
    }
    if (!EnsureGuestBuffers(guest_w_, guest_h_, 0)) {
        ok_ = false;
        return false;
    }
    if (!ui_ready_) {
        forza::ui::VulkanInit ui{instance_, physical_, device_, queue_family_, queue_, overlay_pass_, nimg};
        if (forza::ui::InitVulkan(ui)) ui_ready_ = true;
        else
            LOGW("gpu: the FPS overlay is unavailable, the game carries on without it");
    }
    return true;
}

// The staging buffer and the device image the guest frame is uploaded through,
// one pair per frame in flight (submissions on one queue may overlap, so frame
// N+1 must not rewrite what frame N is still reading). Redone when the guest
// surface size changes; the wait idle first, the pair may still be in use.
bool VulkanBackend::EnsureGuestBuffers(uint32_t w, uint32_t h, uint32_t flight) {
    if (frame_[flight] && frame_w_[flight] == w && frame_h_[flight] == h) return true;
    vkDeviceWaitIdle(device_);
    if (staging_[flight]) vkDestroyBuffer(device_, staging_[flight], nullptr);
    if (staging_mem_[flight]) vkFreeMemory(device_, staging_mem_[flight], nullptr);
    if (frame_[flight]) vkDestroyImage(device_, frame_[flight], nullptr);
    if (frame_mem_[flight]) vkFreeMemory(device_, frame_mem_[flight], nullptr);
    staging_[flight] = VK_NULL_HANDLE;
    staging_mem_[flight] = VK_NULL_HANDLE;
    staging_map_[flight] = nullptr;
    frame_[flight] = VK_NULL_HANDLE;
    frame_mem_[flight] = VK_NULL_HANDLE;
    frame_w_[flight] = frame_h_[flight] = 0;
    VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = bytes;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    FORZA_VK(vkCreateBuffer(device_, &bc, nullptr, &staging_[flight]));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, staging_[flight], &mr);
    uint32_t type = FindMemoryType(physical_, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        LOGE("gpu: no host-visible memory for the upload buffer");
        return false;
    }
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    FORZA_VK(vkAllocateMemory(device_, &ma, nullptr, &staging_mem_[flight]));
    FORZA_VK(vkBindBufferMemory(device_, staging_[flight], staging_mem_[flight], 0));
    FORZA_VK(vkMapMemory(device_, staging_mem_[flight], 0, bytes, 0, (void**)&staging_map_[flight]));
    staging_size_[flight] = bytes;
    VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    im.imageType = VK_IMAGE_TYPE_2D;
    im.format = swap_format_;
    im.extent = {w, h, 1};
    im.mipLevels = 1;
    im.arrayLayers = 1;
    im.samples = VK_SAMPLE_COUNT_1_BIT;
    im.tiling = VK_IMAGE_TILING_OPTIMAL;
    im.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    FORZA_VK(vkCreateImage(device_, &im, nullptr, &frame_[flight]));
    vkGetImageMemoryRequirements(device_, frame_[flight], &mr);
    type = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        LOGE("gpu: no device memory for the frame image");
        return false;
    }
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    FORZA_VK(vkAllocateMemory(device_, &ma, nullptr, &frame_mem_[flight]));
    FORZA_VK(vkBindImageMemory(device_, frame_[flight], frame_mem_[flight], 0));
    frame_w_[flight] = w;
    frame_h_[flight] = h;
    return true;
}

void VulkanBackend::DestroySwapchain() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    framebuffers_.clear();
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    views_.clear();
    for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
    rendered_.clear();
    if (!cmds_.empty()) vkFreeCommandBuffers(device_, pool_, uint32_t(cmds_.size()), cmds_.data());
    cmds_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
    // whatever acquire was in the air died with it; fresh semaphores come with
    // the rebuilt swapchain, a stale signalled one would poison the next take
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        if (acquired_[i]) vkDestroySemaphore(device_, acquired_[i], nullptr);
        acquired_[i] = VK_NULL_HANDLE;
    }
}

void VulkanBackend::ApplyGamma(std::vector<uint8_t>& px) const {
    // BGRA8: B, G, R, A; the ramp is R, G, B
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i] = gamma_[512 + px[i]];
        px[i + 1] = gamma_[256 + px[i + 1]];
        px[i + 2] = gamma_[px[i + 2]];
    }
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    if (!ok_) return;
    if (flags & kClearTarget) {
        gpu::TextureDesc d;
        if (!DescribeSurface(s.rt, d)) return;
        // the D3DCOLOR word only lands byte-correct in the A/X8R8G8B8 linear
        // formats; anything else waits for the renderer of 3-b
        if (d.format != 0x12 && d.format != 0x1E) {
            LOG_FIRST(4, "gpu: clear of a %ux%u format %u target is pending 3-b", d.width, d.height, d.format);
            return;
        }
        uint32_t* base = (uint32_t*)kernel::HostPtr(d.data);
        uint32_t stride = d.pitch / 4;
        if (count && rects) {
            for (uint32_t r = 0; r < count; r++) {
                // D3DRECT: x1, y1, x2, y2, x2/y2 exclusive
                const int32_t* q = (const int32_t*)(rects + size_t(r) * 16);
                uint32_t ax = uint32_t(std::clamp(q[0], 0, int32_t(d.width)));
                uint32_t ay = uint32_t(std::clamp(q[1], 0, int32_t(d.height)));
                uint32_t bx = uint32_t(std::clamp(q[2], 0, int32_t(d.width)));
                uint32_t by = uint32_t(std::clamp(q[3], 0, int32_t(d.height)));
                for (uint32_t y = ay; y < by; y++)
                    for (uint32_t x = ax; x < bx; x++) base[size_t(y) * stride + x] = color;
            }
            return;
        }
        for (uint32_t y = 0; y < d.height; y++)
            for (uint32_t x = 0; x < d.width; x++) base[size_t(y) * stride + x] = color;
    }
    if ((flags & (kClearZ | kClearStencil)) && s.zs.header) {
        gpu::TextureDesc d;
        if (!DescribeSurface(s.zs, d) || !d.linear) return;
        uint8_t* base = (uint8_t*)kernel::HostPtr(d.data);
        uint32_t f = d.format;
        if (f == 0x30) {  // LIN_D16
            uint16_t v = uint16_t(z * 65535.0f + 0.5f);
            for (uint32_t y = 0; y < d.height; y++) {
                uint16_t* row = (uint16_t*)(base + size_t(y) * d.pitch);
                for (uint32_t x = 0; x < d.width; x++) row[x] = v;
            }
        } else if (f == 0x31) {  // LIN_F16
            uint16_t v = Half(z);
            for (uint32_t y = 0; y < d.height; y++) {
                uint16_t* row = (uint16_t*)(base + size_t(y) * d.pitch);
                for (uint32_t x = 0; x < d.width; x++) row[x] = v;
            }
        } else {  // 24-bit depth in 32-bit words, stencil in the top byte
            uint32_t z24 = uint32_t(z * 16777215.0f + 0.5f) & 0xFFFFFF;
            bool with_stencil = flags & kClearStencil;
            for (uint32_t y = 0; y < d.height; y++) {
                uint32_t* row = (uint32_t*)(base + size_t(y) * d.pitch);
                for (uint32_t x = 0; x < d.width; x++)
                    row[x] = (with_stencil ? stencil << 24 : row[x] & 0xFF000000u) | z24;
            }
        }
    }
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!ok_) return;
    LOG_FIRST(8, "gpu: draw prim %u start %u count %u arrives with milestone 3-b", prim, start, count);
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                         uint32_t dst_points_va) {
    if (!ok_) return;
    if (!CopySurfaceMemory(src, dst, count, src_rects_va, dst_points_va))
        LOG_FIRST(4, "gpu: unsupported CopyRects is pending 3-b");
}

void VulkanBackend::Present(const Surface& front) {
    if (!ok_ || !front.header) {
        // setup failed: behave like the null backend, the game thread must not spin
        if (!ok_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    // the Android surface is gone while the app is in the background; park the
    // frame here until it is back, then rebuild around the new surface
    if (!host::Foreground()) host::WaitForeground();
    if (host::SurfaceEpoch() != epoch_ || recreate_next_) {
        epoch_ = host::SurfaceEpoch();
        recreate_next_ = false;
        DestroySwapchain();
        if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface((SDL_Window*)host::Window(), instance_, nullptr, &surface_)) {
            LOGE("gpu: Vulkan surface re-creation failed: %s", SDL_GetError());
            ok_ = false;
            return;
        }
        if (!CreateSurfaceAndSwapchain()) {
            ok_ = false;
            return;
        }
    }
    gpu::TextureDesc d;
    if (!DescribeSurface(front, d)) {
        LOG_FIRST(4, "gpu: present of an undecodable surface %08X", front.header);
        return;
    }
    if (!d.linear || Format(d.format).bits != 32) {
        LOG_FIRST(4, "gpu: present of %ux%u format %u is pending 3-b", d.width, d.height, d.format);
        return;
    }
    if (d.width != guest_w_ || d.height != guest_h_) {
        guest_w_ = d.width;
        guest_h_ = d.height;
    }
    uint32_t flight = flight_;
    flight_ = (flight_ + 1) % kFramesInFlight;
    if (!EnsureGuestBuffers(guest_w_, guest_h_, flight)) {
        ok_ = false;
        return;
    }
    uint32_t pitch = 0;
    std::vector<uint8_t> px;
    DecodeLevel(d, 0, 0, (const uint8_t*)kernel::HostPtr(d.data), nullptr, px, pitch);
    if (px.empty()) {
        LOG_FIRST(4, "gpu: present decode failed for %08X", front.header);
        return;
    }
    if (!gamma_identity_) ApplyGamma(px);
    // the decoder makes BGRA8; a swapchain that came up as RGBA8 needs the red
    // and blue swapped before the upload
    if (swap_format_ == VK_FORMAT_R8G8B8A8_UNORM)
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    if (SubmitFrame(px, flight)) forza::ui::PresentTick();
}

// One frame: upload the decoded guest surface, blit it aspect-fitted onto the
// swapchain image, draw the overlay pass on top, present. True when the frame
// was actually submitted (the FPS counter only counts shown frames).
bool VulkanBackend::SubmitFrame(const std::vector<uint8_t>& px, uint32_t flight) {
    VkFence fence = fences_[flight];
    FORZA_VK_SOFT(vkWaitForFences(device_, 1, &fence, VK_TRUE, ~0ull), "gpu: fence wait failed (%d)");
    uint32_t idx = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, ~0ull, acquired_[flight], VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_ERROR_SURFACE_LOST_KHR) {
        recreate_next_ = true;
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOG_FIRST(8, "gpu: acquire failed (%d)", int(acq));
        return false;
    }
    recreate_next_ |= acq == VK_SUBOPTIMAL_KHR;  // the image is still good, show it and rebuild later
    if (idx >= cmds_.size()) return false;
    VkCommandBuffer cmd = cmds_[idx];
    FORZA_VK_SOFT(vkResetFences(device_, 1, &fence), "gpu: fence reset failed (%d)");
    FORZA_VK_SOFT(vkResetCommandBuffer(cmd, 0), "gpu: command reset failed (%d)");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        LOG_FIRST(8, "gpu: command buffer begin failed");
        return false;
    }
    // the upload: the decoded guest frame straight into the mapped staging buffer
    VkDeviceSize bytes = VkDeviceSize(guest_w_) * guest_h_ * 4;
    if (staging_size_[flight] >= bytes && px.size())
        std::memcpy(staging_map_[flight], px.data(), std::min(VkDeviceSize(px.size()), bytes));
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.subresourceRange = all;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageMemoryBarrier swap_bar = to_transfer;
    swap_bar.image = images_[idx];
    VkImageMemoryBarrier frame_bar = to_transfer;
    frame_bar.image = frame_[flight];
    VkImageMemoryBarrier bars[] = {frame_bar, swap_bar};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, bars);
    VkBufferImageCopy cpy{};
    cpy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpy.imageExtent = {guest_w_, guest_h_, 1};
    vkCmdCopyBufferToImage(cmd, staging_[flight], frame_[flight], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    VkImageMemoryBarrier to_src = frame_bar;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_src);
    // the letterbox: the guest aspect kept inside the screen, black around it
    VkClearColorValue black{};
    vkCmdClearColorImage(cmd, images_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    float scale = std::min(extent_.width / float(guest_w_), extent_.height / float(guest_h_));
    uint32_t dw = std::min(uint32_t(guest_w_ * scale + 0.5f), extent_.width);
    uint32_t dh = std::min(uint32_t(guest_h_ * scale + 0.5f), extent_.height);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {int32_t(guest_w_), int32_t(guest_h_), 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[0] = {int32_t((extent_.width - dw) / 2), int32_t((extent_.height - dh) / 2), 0};
    bl.dstOffsets[1] = {int32_t((extent_.width - dw) / 2 + dw), int32_t((extent_.height - dh) / 2 + dh), 1};
    vkCmdBlitImage(cmd, frame_[flight], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[idx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    VkImageMemoryBarrier to_color = swap_bar;
    to_color.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_color);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = overlay_pass_;
    rb.framebuffer = framebuffers_[idx];
    rb.renderArea = {0, 0, extent_.width, extent_.height};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    forza::ui::Record(cmd);
    vkCmdEndRenderPass(cmd);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG_FIRST(8, "gpu: command buffer end failed");
        return false;
    }
    // the first touch of the taken image is the transfer below, so that is the
    // stage the acquire semaphore waits in
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_[flight];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[idx];
    FORZA_VK_SOFT(vkQueueSubmit(queue_, 1, &si, fence), "gpu: submit failed (%d)");
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &idx;
    VkResult shown = vkQueuePresentKHR(queue_, &pi);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_ERROR_SURFACE_LOST_KHR || shown == VK_SUBOPTIMAL_KHR)
        recreate_next_ = true;
    else if (shown != VK_SUCCESS)
        LOG_FIRST(8, "gpu: present failed (%d)", int(shown));
    return true;
}

void VulkanBackend::SetGamma(const uint8_t* ramp) {
    if (!ramp) return;
    std::memcpy(gamma_, ramp, sizeof(gamma_));
    gamma_identity_ = true;
    for (int i = 0; i < 256; i++)
        if (gamma_[i] != i || gamma_[256 + i] != i || gamma_[512 + i] != i) {
            gamma_identity_ = false;
            break;
        }
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
