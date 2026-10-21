# Architecture

## Layers

Dependencies point in one direction only. `core` knows nothing about Android,
Vulkan or SDL. `runtime` knows nothing about Android. `platform`, `gfx` and
`audio` adapt the runtime to the device. `game` and `ui` sit on top.

```
            app/ (Kotlin/Java: Activity, permissions, importer)
                              │ JNI / SDLActivity
┌─────────────────────────────┴─────────────────────────────┐
│                    native/ (one libmain.so)               │
│                                                           │
│  game/      hooks, patches, game_info, Forza specifics    │
│  ui/        ImGui overlay and options menu                │
│  platform/  SDL3 window, lifecycle, storage, input, thermal│
│  gfx/       Vulkan backend, D3D8 state cache, shaders     │
│  audio/     SDL3/AAudio output                            │
│  runtime/   SDK kernel + XDK HLE + generated/game         │
│  core/      logging, config, small utilities               │
└───────────────────────────────────────────────────────────┘
                              │
                    sdk/ (XboxXBE-Recomp, CI checkout)
```

## Runtime data flow

1. `app::FindGame` finds `default.xbe` (or an ISO) in
   `/storage/emulated/0/Forza`, the app's `assets/` or an SD card, and checks
   its SHA-1 against `kImageSha1` (written by CMake from the build-time XBE).
2. `kernel::Boot` maps the XBE, resolves kernel imports, creates the main
   guest thread and starts the recompiled entry point.
3. The recompiled code runs the Xbox kernel/XDK calls as host functions
   (`GUEST`/`SYM` from `xdk/hle.h`).
4. `xdk/d3d8.cpp` records D3D8 state and calls `gpu::Backend`
   (`Clear`/`Draw`/`Copy`/`Present`). The Vulkan backend translates it.
5. `xdk/dsound.cpp` mixes audio and calls `audio::StartOutput`; SDL3/AAudio
   plays it.
6. `input::Poll` reads SDL gamepads (and, later, touch/gyro) for `xapi.cpp`.
7. `app::Pace` enforces the FPS cap; `app::Save` writes `forza.toml`.

## Build pipeline (CI)

```
FORZA_XBE_URL ──► assets/default.xbe
                        │
                        ├─► xbe_symbols ──► config/game.toml ([symbols], [data], partitions)
                        │
                        └─► xbe_recomp ──► generated/game/*.cpp
                                                  │
sdk/runtime (portable subset) ────────────────────┤
native/* (host) ──────────────────────────────────┼─► NDK arm64 ─► libmain.so ─► APK
third_party/SDL ──────────────────────────────────┘
```

- `tools/` builds `xbe_recomp` and `xbe_symbols` with the host compiler.
- `config/game.toml` is the recompiler input. `partitions` controls how many
  `x86_recomp.N.cpp` files are generated; CMake reads it.
- `generated/` is never committed. `sdk/`, `third_party/`, `assets/`, `iso/`
  are gitignored.

## Android host

- `ForzaActivity extends SDLActivity`; `getLibraries()` returns
  `{"SDL3", "main"}`.
- SDL3 owns the window, event loop, gamepads and audio stream.
- `native/platform/main.cpp` runs the SDL loop and starts `kernel::Boot`.
- `native/platform/app.cpp` implements the `app::` interface (settings,
  display, paths, pacing).
- `native/platform/game_files.cpp` implements `app::FindGame`/`Sha1`.
- `native/platform/input.cpp` implements `input::`.
- `native/audio/output.cpp` implements `audio::StartOutput`.
- `native/gfx/backend.cpp` implements `gpu::CreateBackend` and
  `gpu::CreateThreadedBackend`.
- `native/game/game_info.cpp` defines `app::kGame` for Forza.

## Backend milestones

- **M1 (now):** `NullBackend` accepts every call, presents nothing. The game
  can boot and log; the window shows black.
- **M2:** Vulkan instance/device/swapchain + ImGui overlay (log/FPS).
- **M3:** render passes, pipelines, draws, textures, shader translation.
- **M4+:** state cache, pipeline cache, ASTC/ETC2, dynamic resolution,
  thermal governor.

## Why this shape

- Reusing the SDK's portable kernel/XDK means we only own the platform and
  graphics translation, not a second emulator.
- A single `libmain.so` avoids linker-order and symbol-visibility problems
  between the generated code (weak recompiled functions + strong HLE
  overrides) and the runtime.
- `native/` modules are separate CMakeLists but append to the same target,
  so the build stays parallel and incremental without fighting static
  library dead-stripping.
