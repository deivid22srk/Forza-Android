// Milestone 1 graphics: a backend that accepts every D3D8 call and draws
// nothing. It lets the game boot, run its logic and log before the Vulkan
// renderer exists. The Vulkan backend replaces CreateBackend later.
#include <chrono>
#include <memory>
#include <thread>

#include "gpu/backend.h"

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

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }

// The null backend has no GPU work to overlap: the game thread drives it
// directly. The Vulkan backend will wrap its own render thread here.
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return inner; }

}  // namespace gpu
