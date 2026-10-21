// The backend factory: the Vulkan renderer of milestone 3, falling back to the
// no-op backend when Vulkan cannot be set up, so the game boots and logs the
// same way it did in milestone 1.
#include <chrono>
#include <memory>
#include <thread>

#include "core/log.h"
#include "gpu/backend.h"
#include "vulkan.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t, uint32_t) override { return true; }

    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}

    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {}

    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {}

    void Present(const Surface&) override {
        // Nothing is shown; keep the title from spinning a core at 100%.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void Sync(uint32_t, uint32_t) override {}

    void SyncLate(uint32_t) override {}

    void Flush() override {}

    void SetGamma(const uint8_t*) override {}
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    if (std::unique_ptr<Backend> vulkan = CreateVulkanBackend()) return vulkan;
    LOGE("gpu: Vulkan unavailable, falling back to the null backend");
    return std::make_unique<NullBackend>();
}

// The null backend has no GPU work to overlap: the game thread drives it
// directly. The Vulkan backend records and presents from the game thread too.
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return inner; }

}  // namespace gpu
