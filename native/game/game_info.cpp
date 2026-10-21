#include "app/app.h"

// What the runtime asks of the game; strings must be set, hooks may stay null.
// Forza-specific hooks and options are added here (and in hooks.cpp), never in
// generated/.
const app::GameInfo app::kGame = {
    .title = L"Forza Motorsport",
    .short_title = L"Forza",
    .name = L"Forza",
    .log = "forza",
    .settings = "forza.toml",
    .splash = nullptr,
    .missing =
        L"Put your legally acquired Forza Motorsport files in /storage/emulated/0/Forza: default.xbe with its "
        L"media folder, or the game ISO.",
    .images = {{"default.xbe", "Forza"}},
    .options = {},
    .load = nullptr,
    .save = nullptr,
    .init = nullptr,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = nullptr,
    .pad = nullptr,
    .rumble = nullptr,
};
