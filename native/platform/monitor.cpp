// The boot-long signals the black screen needs. The title's background
// installer (BKGDINST) copies the media cabs off the disc into the hard disk
// on its first runs, and on a phone that takes minutes of nothing in the log:
// the files it writes go through NtWriteFile, which does not trace. A watch on
// the hdd folder turns that into a paced progress line, and it also settles
// whether a silent stretch is the installer working or the guest standing
// still. The tree holds every partition the kernel mounts, so a save or the
// persisted display show up in the numbers too: the line measures, it does not
// attribute.
//
// The other half of the signal is the frame heartbeat in app::Pace.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "host.h"

namespace fs = std::filesystem;

namespace monitor {

namespace {

// The hdd kernel::Boot was given; Z: (hard disk partition 5) lives under it.
fs::path Hdd() { return app::ExeDir() / "hdd"; }

// Bytes and files of the whole tree, 0/0 while the folder is not there yet.
// A manual iterator loop on purpose: the range-for's hidden operator++ throws
// when a folder the installer is churning away disappears mid-walk, and a
// throw on this detached thread would terminate the whole process.
void TreeSize(const fs::path& dir, uint64_t& bytes, uint64_t& files) {
    bytes = files = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code fe;
        if (it->is_regular_file(fe)) {
            uintmax_t size = it->file_size(fe);
            if (!fe) {  // the error result -1 would poison the baseline forever
                bytes += uint64_t(size);
                files++;
            }
        }
        it.increment(ec);
    }
}

void Watch() {
    const fs::path dir = Hdd();
    LOGI("hdd: watching %s", dir.string().c_str());
    uint64_t last_bytes = 0, last_files = 0;
    TreeSize(dir, last_bytes, last_files);
    auto logged = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        uint64_t bytes = 0, files = 0;
        TreeSize(dir, bytes, files);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - logged).count();
        if (bytes != last_bytes) {
            LOGI("hdd: %llu files, %.1f MiB (%+.1f MiB in %.0f s)", (unsigned long long)files, bytes / 1048576.0,
                 (double(bytes) - double(last_bytes)) / 1048576.0, secs);
            last_bytes = bytes;
            last_files = files;
            logged = std::chrono::steady_clock::now();
        } else {
            if (files != last_files) last_files = files;  // keep the count honest without logging
            if (secs >= 300) {
                // the watch is alive and the hard disk is standing still, so silence has a witness
                LOGI("hdd: quiet for %.0f s (%llu files, %.1f MiB)", secs, (unsigned long long)last_files,
                     last_bytes / 1048576.0);
                logged = std::chrono::steady_clock::now();
            }
        }
    }
}

}  // namespace

void StartHddWatch() { std::thread(Watch).detach(); }

}  // namespace monitor
