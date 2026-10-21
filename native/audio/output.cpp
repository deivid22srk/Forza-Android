// The SDK mixer's stereo float output through an SDL3 audio stream (AAudio
// underneath on Android 8+). SDL pulls blocks on its audio thread; the mixer
// renders straight into a fixed buffer, so no allocation happens per frame.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr uint32_t kBlock = kRate / 100;  // 10 ms

struct Pump {
    std::function<void(float*, uint32_t)> render;
    std::vector<float> buf = std::vector<float>(kBlock * 2);
};

void SDLCALL Feed(void* user, SDL_AudioStream* stream, int additional, int) {
    auto& p = *static_cast<Pump*>(user);
    constexpr int kFrameBytes = 2 * sizeof(float);
    int want = std::max(additional, 0) / kFrameBytes;
    while (want > 0) {
        int frames = std::min(want, int(kBlock));
        p.render(p.buf.data(), uint32_t(frames));
        SDL_PutAudioStreamData(stream, p.buf.data(), frames * kFrameBytes);
        want -= frames;
    }
}

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    // SDL owns the stream for the life of the process; a second StartOutput
    // would replace the mixer under the audio thread.
    static SDL_AudioStream* stream = nullptr;
    if (stream) return;
    static Pump pump;
    pump.render = std::move(render);

    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, &pump);
    if (!stream) {
        LOGE("audio: no output device, no sound: %s", SDL_GetError());
        return;
    }
    LOGI("audio: %d Hz stereo float through %s", int(kRate), SDL_GetCurrentAudioDriver());
    if (!SDL_ResumeAudioStreamDevice(stream)) LOGE("audio: could not start the device: %s", SDL_GetError());
}

}  // namespace audio
