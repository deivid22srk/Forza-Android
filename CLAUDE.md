# CLAUDE.md

Guidance for AI/code agents working in this repository.

## What this is

An unofficial Android (arm64) port of the original Xbox game *Forza
Motorsport* (2005), made with the [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)
SDK. The game's x86 code is statically recompiled to C++ on a CI host, then
built for Android with the NDK together with the SDK's portable runtime and
this repo's Android host. Window/input/audio use SDL3; the planned renderer is
Vulkan 1.1. The repo ships no game assets: `assets/`, `iso/`, `generated/` and
`sdk/` are gitignored.

## Build: CI only

**Do not build the APK locally and do not run the recompiler locally.** The
only supported build is `.github/workflows/build.yml` (GitHub Actions,
Ubuntu runner). Locally you may read/write code and run linters only:

```bash
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.h')
yamllint .github/workflows/build.yml
actionlint
```

The workflow:

1. Checks out the SDK into `sdk/` at the revision in `ci/sdk-revision.txt`.
2. Downloads `default.xbe` (or an ISO) from the `FORZA_XBE_URL` secret, checks
   `FORZA_XBE_SHA256` if set.
3. Builds `tools/` (`xbe_symbols`, `xbe_recomp`) with the host compiler.
4. Runs `xbe_symbols assets/default.xbe sdk/xdk --library libmain.so --out config/game.toml`
   (only if `config/game.toml` has no `[symbols]` yet).
5. Runs `xbe_recomp config/game.toml --xdk sdk/xdk` → `generated/game/`.
6. Runs `./gradlew assembleRelease` with `-PforzaStub=false`.
7. Uploads the APK.

Without `FORZA_XBE_URL` the workflow builds a **stub** APK
(`-PforzaStub=true`, `FORZA_STUB=ON`): SDL3 window + Java dialog, no game
code. This keeps `main` green before the secret exists.

## SDK layout and what we replace

The SDK lives in `sdk/` (not committed). We compile its portable runtime but
drop the Windows-only pieces:

```
runtime/app/*            dropped (Android host in native/platform)
runtime/core/input.cpp   dropped (native/platform/input.cpp)
runtime/gpu/d3d12.cpp    dropped (native/gfx backend)
runtime/gpu/render_thread.cpp  dropped (native/gfx threaded wrapper)
runtime/audio/xaudio2.cpp      dropped (native/audio/output.cpp)
runtime/xdk/xnet.cpp     dropped for milestone 1 (Windows sockets)
```

Everything else from `runtime/` is compiled: kernel, CPU dispatch, XBE,
D3D8/DirectSound HLE, shader/vertex/texture helpers, Bink, CRT math, XAPI.

We implement these interfaces:

- `app::` from `runtime/app/app.h` — `native/platform/app.cpp`,
  `native/platform/game_files.cpp`.
- `input::` from `runtime/core/input.h` — `native/platform/input.cpp`.
- `audio::StartOutput` from `runtime/audio/audio.h` — `native/audio/output.cpp`.
- `gpu::CreateBackend` / `gpu::CreateThreadedBackend` from
  `runtime/gpu/backend.h` — `native/gfx/backend.cpp`.
- `app::kGame` from `runtime/app/app.h` — `native/game/game_info.cpp`.

## Generated code

`generated/game/` is produced by `xbe_recomp`; **never edit it**. It contains:

- `x86_recomp.N.cpp` for N < `partitions` (from `config/game.toml`).
- `x86_func_mapping.cpp`, `x86_recomp_shared.h`, `image_symbols.h`,
  `hook_exports.h`.
- `.def` files (Windows linker only; unused on Android).

Host code refers to XDK symbols through `SYM(Name)` and `GUEST(Name)` from
`xdk/hle.h`. To replace a recompiled function, define a strong
`X86_FUNC(sub_XXXXXXXX)` in `native/game/` and call `__imp__sub_XXXXXXXX` for
the original. To add an instruction hook, add `[[hook]]` to
`config/game.toml`, define `void Name(X86Context&, uint8_t*)`, and recompile.

## Forza image facts (from the user's XBE)

- XBE title: *Forza Motorsport*, title ID `0x4D53006E`, retail (not debug).
- XDK build: **5849** (matches `sdk/xdk/5849`).
- Entry point: `0x00268EB5` (header field is XORed with `0xA8FC57AB`).
- Kernel thunk: `0x00485D80`.
- Libraries: `XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XBOXKRNL, XACTENG,
  XVOICE, VOICMAIL, XONLINES, LIBCPMT, LIBCMT`.
- Sections include `.text`, `FZGINIT`, `FZFULLUI`, `BINK*`, `D3D`, `D3DX`,
  `XGRPH`, `DSOUND`, `WMADEC`, `XACTENG`, `XONLINE`, `XNET`, `XPP`, `.rdata`,
  `.data`, `DDERR`, `DOLBY`, `XON_RD`, `BINKDATA`, `$$XTIMAGE`, `BKGDINST`,
  `HASHES`, `.XTLID`.

See `docs/GAPS.md` for the coverage analysis.

## Conventions

- C++20. Native code is compiled with `-fsigned-char -fno-strict-aliasing`,
  matching x86 semantics.
- Only `arm64-v8a`, `minSdk 29`. 16 KB page alignment.
- Never touch `generated/`; change `config/game.toml`, `native/game/` or
  `native/runtime/` instead.
- Keep the dependency direction: `core` has no Android/Vulkan/SDL dependency;
  `runtime` has no Android dependency; `platform`/`gfx`/`audio` adapt.
- Comments explain *why*, lowercase, terse (match the SDK style).
- Commit messages: `tipo(escopo): descrição` (e.g. `feat(gfx): add null backend`).
- Do not commit secrets, tokens, game files, `generated/`, `sdk/`,
  `third_party/` or keystores.

## Pitfalls

- **Token handling:** never write a GitHub token into a file, workflow, remote
  URL or `.git/config`. Use `GH_TOKEN` in the environment and
  `gh auth setup-git`.
- The XBE header XORs entry point and kernel thunk with retail/debug keys;
  use `sdk/runtime/xbe/xbe.h` semantics if you parse it.
- `xbe_symbols` rewrites `config/game.toml` with `[symbols]`, `[data]` and the
  partition count. Re-run the recompiler after changing hooks/symbols/ABI.
- The Android `libmain.so` is loaded by SDL's `SDLActivity`; `getLibraries()`
  must return `{"SDL3", "main"}`.
- The runtime's `log.cpp` writes to stderr; `native/core/logcat.cpp` pipes it
  to logcat.
- `kernel::Boot` expects an XBE whose entry matches the recompiled
  `dispatch::EntryPoint()`; a different region/revision will be refused.
- `xdk/d3d8.cpp` uses `app::SplashFirstFrame`, `HandingOver`,
  `FinishHandover`, `Pace`, `Widescreen` — all implemented in
  `native/platform/app.cpp`.
- `xdk/dsound.cpp` calls `audio::StartOutput`; if audio fails it logs and the
  game keeps running silently.
