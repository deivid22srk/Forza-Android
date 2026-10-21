# Third-party components

This project is built on the following MIT-licensed projects. Their licence
notices are reproduced here as required by the MIT licence.

## XboxXBE-Recomp

- Upstream: https://github.com/TGP482/XboxXBE-Recomp
- Copyright (c) 2026 TGP482
- Licence: MIT

The recompiler, the portable Xbox runtime (kernel, XDK libraries, Direct3D 8
translation), the `xbe_symbols` tool and the generated-code conventions come
from this project. The SDK is checked out by CI into `sdk/` and is not
vendored here.

## Black for Android

- Upstream: https://github.com/RanggaCasper/Black-Android
- Copyright (c) 2026 Rangga Casper
- Licence: MIT

The Android host architecture (SDL3 activity, Vulkan backend shape, game-file
discovery, Gradle pipeline) is closely modelled on this port. Some small
utility files (`native/platform/compat/crt.h`, the Gradle wrapper and
`tools/xdvdfs.py`) are reused or adapted from it.

## SDL

- Upstream: https://github.com/libsdl-org/SDL
- Licence: zlib

## toml++

- Upstream: https://github.com/marzer/tomlplusplus
- Licence: MIT

## Zydis

- Upstream: https://github.com/zyantific/zydis
- Licence: MIT

## glslang (future Vulkan backend)

- Upstream: https://github.com/KhronosGroup/glslang
- Licence: BSD-3-Clause / Apache-2.0

*Forza Motorsport* is a trademark of Microsoft Corporation. This project is
not affiliated with or endorsed by Microsoft or Turn 10 Studios.
