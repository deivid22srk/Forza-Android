// The app:: interface the SDK runtime asks of the host: settings, display,
// paths and frame pacing. Game discovery is in game_files.cpp.
#include <SDL3/SDL.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"

#ifndef FORZA_COMMIT
#define FORZA_COMMIT "dev"
#endif

namespace app {

namespace {

namespace fs = std::filesystem;

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

fs::path File() { return ExeDir() / kGame.settings; }

std::string Property(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

float RefreshRate() {
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? m->refresh_rate : 60.0f;
}

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", std::string(kModes[uint32_t(s.display_mode)])},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else
            t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

Settings& Get() {
    // a phone starts at about 720p, 30 FPS, no vsync; the options menu changes
    // these and Save writes them to forza.toml
    static Settings s = [] {
        Settings d;
        d.render_percent = 67;
        d.fps_cap = 30;
        d.vsync = false;
        return d;
    }();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else
            *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s", kGame.title, IMAGE_XBE, date, FORZA_COMMIT);
    LOGR("Path: %s", ExeDir().string().c_str());
    LOGR("Device: %s %s, Android %s (API %d)", Property("ro.product.manufacturer").c_str(),
         Property("ro.product.model").c_str(), Property("ro.build.version.release").c_str(),
         SDL_GetAndroidSDKVersion());
    LOGR("CPU: %s (%d threads)", Property("ro.soc.model").c_str(), SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Display: %ux%u, %.0f Hz", desk.width, desk.height, RefreshRate());
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() {
    int w = 0, h = 0;
    if (host::Window()) {
        host::WindowPixels(w, h);
        if (w > 0 && h > 0) return {uint32_t(std::max(w, h)), uint32_t(std::min(w, h))};
    }
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return {1920, 1080};
    uint32_t ww = uint32_t(float(m->w) * m->pixel_density);
    uint32_t hh = uint32_t(float(m->h) * m->pixel_density);
    return {std::max(ww, hh), std::min(ww, hh)};
}

Mode Output() {
    // the window is the screen: the resolution setting is the screen's own
    return Desktop();
}

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = float(Output().height) * (float(Get().render_percent) / 100.0f);
    return guest_height ? std::max(h / float(guest_height), 0.1f) : 1.0f;
}

// App-specific external storage: the player can reach logs, saves and settings
// over USB.
fs::path ExeDir() {
    static const fs::path dir = [] {
        const char* p = nullptr;
        if (SDL_GetAndroidExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE)
            p = SDL_GetAndroidExternalStoragePath();
        if (!p) p = SDL_GetAndroidInternalStoragePath();
        return fs::path(p ? p : ".");
    }();
    return dir;
}

int Instance() { return 0; }
void* Window() { return host::Window(); }

bool PrepareGameCode(const GameFiles&) { return true; }

// No splash and no relaunch into another executable on Android.
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app
