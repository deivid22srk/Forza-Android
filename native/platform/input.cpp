// Controllers through SDL on Android: read into the console controller's
// report. The main thread hands over plug events, because Android delivers
// input there; the guest threads only poll.
#include "core/input.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// A controller takes the lowest free port when it is plugged in and keeps it
// until it is unplugged.
std::mutex g_lock;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open",
         name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

void Read(SDL_Gamepad* pad, Pad& p) {
    p.connected = true;
    constexpr SDL_GamepadButton kButtons[] = {SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                              SDL_GAMEPAD_BUTTON_DPAD_LEFT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                                              SDL_GAMEPAD_BUTTON_START,      SDL_GAMEPAD_BUTTON_BACK,
                                              SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
    // A B X Y black white
    constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
                                             SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
                                             SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
    auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
    // 0 to 32767 into 0 to 255
    p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
    p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
    // SDL's sticks have Y down; not plain negation: -32768 has no positive
    p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
}

}  // namespace

void Init() {
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

void OnPadEvent(const SDL_Event& e) {
    std::lock_guard lock(g_lock);
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
        RemovePad(e.gdevice.which);
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) Read(pad, p);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {{SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},
                                                                       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
                                                                       {SDL_GAMEPAD_BUTTON_WEST, 0x4000},
                                                                       {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// The motors run until set again.
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
