# Decisions

## ADR-001 — Static recompilation, not emulation

Use XboxXBE-Recomp to translate the game's x86 to C++ and compile it for
arm64. No interpreter/JIT emulator. Rationale: the user's requirement and the
Black port's proven approach; better performance and a native APK.

## ADR-002 — Reuse the SDK's portable runtime

Compile the SDK's kernel/XDK/D3D8/DirectSound sources, dropping only the
Windows-specific pieces (`runtime/app`, `core/input.cpp`, `gpu/d3d12`,
`gpu/render_thread`, `audio/xaudio2`, `xdk/xnet` for now). Implement the
`app::`, `input::`, `audio::` and `gpu::Backend` interfaces in `native/`.
Rationale: the SDK is the source of truth for Xbox behaviour; a fork would
drift.

## ADR-003 — Null graphics backend first

Milestone 1 ships a `NullBackend` so the APK builds, opens and logs before the
Vulkan backend exists. The Vulkan backend is milestone 3. Rationale: get the
CI/recompilation pipeline green and catch integration errors early without
blocking on a 3k-line renderer.

## ADR-004 — CI-only builds

The APK and the recompiler run only in GitHub Actions. Local development is
read/write/lint only. Rationale: the user's requirement; reproducible builds;
no game assets on developer machines beyond the user's own copy.

## ADR-005 — XBE via secret URL, never committed

The workflow downloads `default.xbe` from `FORZA_XBE_URL` and never commits
it. The recommended source is a separate private GitHub repository holding
only the game file; `FORZA_XBE_TOKEN` (read-only, sent only to GitHub hosts)
authenticates that download. `assets/`, `iso/`, `generated/` are gitignored.
Without `FORZA_XBE_URL`, a stub APK is built. Rationale: legal and repo-size
constraints.

## ADR-006 — Private repository

The generated C++ and the APK contain code derived from the game. Keep the
repo private and do not publish releases without an explicit decision.

## ADR-007 — Modular CMake, single shared library

Each `native/<module>` has its own CMakeLists but appends sources to one
`main` target. Rationale: the generated code and HLE overrides rely on weak
symbols and link order; static libraries would dead-strip them. Modules keep
the source tree organized.

## ADR-008 — SDL3 for window/input/audio

Use SDL3 (as the SDK and Black do) instead of raw Android APIs. SDL3 gives a
portable event loop, gamepad support, audio streams (AAudio underneath) and
the `SDLActivity` Java glue.

## ADR-009 — Vulkan 1.1 baseline

Target Vulkan 1.1 without requiring dynamic rendering or timeline semaphores,
because Mali-G52 and Adreno 619 drivers on Android 10/11 may lack them. Use
explicit render passes and a conservative feature set; enable extensions when
present.

## ADR-010 — arm64 only, armv8-a baseline

Only `arm64-v8a`; `-march=armv8-a` (no optional extensions), `-mtune` for
Cortex-A55 by default. Rationale: G80/480+ support; avoid SIGILL on older
cores. ThinLTO/PGO are opt-in until the build is stable.
