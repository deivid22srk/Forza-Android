// The game on Android: its files or a disc image (.iso) in the Forza folder on
// the phone's storage or on an SD card (all files access granted), or in the
// app's own assets folder. The XBE must be the one this build was recompiled
// from; app::FindGame waits until it appears.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the XBE recompiled into this library, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// The shared storage's folder for the game, readable with all files access:
// the files or the image in it, or in its assets folder.
constexpr const char* kShared = "/storage/emulated/0/Forza";
constexpr const char* kVolumes = "/storage";

// The Forza folder on each SD card or USB stick Android has mounted,
// /storage/<volume id>/Forza; all files access reads them like the phone's own
// storage.
std::vector<fs::path> RemovableFolders() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(kVolumes, ec)) {
        std::string name = e.path().filename().string();
        if (name == "emulated" || name == "self" || !e.is_directory(ec)) continue;
        out.push_back(e.path() / "Forza");
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<fs::path> AssetFolders() {
    std::vector<fs::path> out = {fs::path(kShared), fs::path(kShared) / "assets"};
    for (const fs::path& card : RemovableFolders()) {
        out.push_back(card);
        out.push_back(card / "assets");
    }
    out.push_back(ExeDir() / "assets");
    return out;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

// No game yet: the activity's dialog tells the player where it goes and asks
// for access; this waits a moment before looking again, keeping SDL's events
// moving.
void WaitForGame() {
    static bool logged = false;
    if (!logged) {
        std::error_code ec;
        fs::is_directory(kShared, ec);
        std::string cards;
        for (const fs::path& card : RemovableFolders()) cards += ", " + card.string();
        LOGE("game: no game files in %s%s or %s%s, waiting for them", kShared, cards.c_str(),
             (ExeDir() / "assets").c_str(), ec == std::errc::permission_denied ? ", no access to the first" : "");
        logged = true;
    }
    for (int i = 0; i < 10; i++) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) {
                logx::Flush();
                std::_Exit(0);
            }
        SDL_Delay(100);
    }
}

bool AskOtherRelease(const fs::path& name) {
    std::string text = name.filename().string() +
                       " is a different release of the game than this build was made from, and will most likely "
                       "crash.\n\nStart it anyway?";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
        {0, 1, "Yes"},
    };
    std::string title = Utf8(kGame.short_title);
    SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, title.c_str(), text.c_str(), 2, buttons, nullptr};
    int answer = 0;
    return SDL_ShowMessageBox(&box, &answer) && answer == 1;
}

// 1 found, 0 none there, -1 the player turned down another release
int Search(GameFiles& game) {
    // The first copy with this XBE wins, extracted files before any image;
    // another release is kept to offer.
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    auto consider = [&](const fs::path& assets, const fs::path& name, std::vector<uint8_t> xbe,
                        std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool match = sha == kImageSha1;
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(),
             match ? "" : ", another release");
        if (match) {
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    std::error_code ec;
    for (const fs::path& assets : AssetFolders()) {
        if (!fs::is_directory(assets, ec)) continue;
        std::vector<uint8_t> xbe;
        if (ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, assets / IMAGE_XBE, std::move(xbe), nullptr))
            return 1;

        std::vector<fs::path> isos;
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
        std::sort(isos.begin(), isos.end());
        for (const fs::path& iso : isos) {
            auto disc = std::make_unique<kernel::Disc>();
            std::string error;
            if (!disc->Open(iso, error)) {
                LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
                continue;
            }
            if (!disc->Load(IMAGE_XBE, xbe)) {
                LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
                continue;
            }
            LOGR("Game: image %s, %s %zu bytes", iso.string().c_str(), IMAGE_XBE, xbe.size());
            if (consider(assets, iso, std::move(xbe), std::move(disc))) return 1;
        }
    }

    if (other_name.empty()) return 0;
    if (!AskOtherRelease(other_name)) return -1;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return 1;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game) {
    for (;;) {
        int found = Search(game);
        if (found) return found > 0;
        WaitForGame();
    }
}

}  // namespace app
