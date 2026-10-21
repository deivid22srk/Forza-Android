#pragma once
#include <memory>

#include "gpu/backend.h"

namespace gpu {

// The milestone 3-a renderer: surface, swapchain, the guest's clears and
// copies, and the present of the front surface. Draws arrive with the shader
// translation in 3-b. Null on failure, the factory falls back to the no-op
// backend.
std::unique_ptr<Backend> CreateVulkanBackend();

}  // namespace gpu
