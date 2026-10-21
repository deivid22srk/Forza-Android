// The SDK's log.cpp drops debug, info and warn lines in a release build, and
// the CI ships release: the whole boot (kernel::Boot progress, NtCreateFile
// failures, unimplemented kernel imports) was invisible on the device, and the
// black screen could not be diagnosed from its log. This replacement keeps the
// logx API but makes the sinks honest: stderr, which the logcat pipe forwards,
// carries info and up in every build; the file carries warn and up in release
// and everything in a debug build, so it stays small on long game sessions
// while keeping the warnings milestone 2 depends on.
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

constexpr int kKeep = 30;

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 ||
        name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "report"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    // report lines stay bare (the boot banner reads as plain text), the rest
    // carry their level and thread, which a multi-threaded guest needs
    char line[4352];
    int n = lv == Level::Report
                ? std::snprintf(line, sizeof(line), "[%9.3f] %s\n", t, msg)
                : std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg);
    n = std::min(n, int(sizeof(line)) - 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    // stderr feeds logcat: debug only in a debug build, the recompiled code can
    // emit it in hot paths; info and up always, the boot must be readable
    if (kVerbose || lv >= Level::Info) std::fwrite(line, 1, size_t(n), stderr);
    if (g_file) {
        if (kVerbose || lv >= Level::Warn) std::fwrite(line, 1, size_t(n), g_file);
        if (lv >= Level::Warn) std::fflush(g_file);
    }
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
