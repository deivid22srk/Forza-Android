#!/usr/bin/env bash
# Recompiles assets/default.xbe into generated/game. Run on the build host
# (CI: Ubuntu), never on the phone and never with the game files committed.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="${FORZA_SDK_DIR:-$root/sdk}"
xbe="$root/assets/default.xbe"

[ -f "$xbe" ] || { echo "missing $xbe; run ci/download-xbe.sh first" >&2; exit 1; }
[ -f "$sdk/cmake/XbeRecomp.cmake" ] || { echo "missing the XboxXBE-Recomp SDK at $sdk" >&2; exit 1; }

build="$root/build/tools"
cmake -S "$root/tools" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDK_DIR="$sdk"
cmake --build "$build" --parallel

# xbe_symbols names the XBE's symbols and writes the partitions count that the
# Android CMakeLists reads. --library is the executable the hooks are exported
# from; on Android they all live in libmain.so, so the name is only a label.
"$build/xbe_symbols" "$xbe" "$sdk/xdk" \
    --library libmain.so \
    --name forza \
    --out "$root/config/game.toml"

"$build/xbe_recomp" "$root/config/game.toml" "$xbe" "$root/generated/game" --xdk "$sdk/xdk"

test -f "$root/generated/game/x86_func_mapping.cpp"
echo "recompiled $xbe into $root/generated/game"
