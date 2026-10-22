#!/usr/bin/env bash
# Builds libshaderc for the game ABI and installs it under FORZA_SHADERC_DIR in
# the layout the graphics CMake expects. Google stopped bundling shaderc with
# the NDK (verified absent from the r27b/r27c and r28.2 packages), and milestone
# 3-b compiles the game's generated HLSL with it on the device, so the CI builds
# it from source. The revision comes from ci/shaderc-revision.txt and the DEPS
# file of that revision pins glslang and SPIRV-Tools, so one SHA fixes the whole
# shader toolchain.
#
# Output layout (mirrors the old NDK bundle, which the CMake detection knows):
#   $FORZA_SHADERC_DIR/include/shaderc/shaderc.h
#   $FORZA_SHADERC_DIR/libs/c++_static/<abi>/libshaderc.a
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ABI="${FORZA_SHADERC_ABI:-arm64-v8a}"
PLATFORM="${FORZA_SHADERC_PLATFORM:-android-29}"
OUT="${FORZA_SHADERC_DIR:?FORZA_SHADERC_DIR must name the output directory}"
REV="$(tr -d '[:space:]' < "$ROOT/shaderc-revision.txt")"

# The NDK to build with: the one Gradle uses for the APK, so the static objects
# and the app's libc++ headers come from the same toolchain. NDK_VERSION names
# the exact one the workflow installs; the glob is the local fallback.
if [[ -z "${ANDROID_NDK:-}" ]]; then
    if [[ -n "${NDK_VERSION:-}" && -d "${ANDROID_HOME:-/nonexistent}/ndk/$NDK_VERSION" ]]; then
        ANDROID_NDK="$ANDROID_HOME/ndk/$NDK_VERSION"
    else
        for d in "${ANDROID_HOME:-/nonexistent}"/ndk/*; do
            if [[ -d "$d" ]]; then ANDROID_NDK="$d"; fi
        done
    fi
fi
: "${ANDROID_NDK:?could not locate an NDK (set ANDROID_NDK or ANDROID_HOME)}"
echo "shaderc build: NDK=$ANDROID_NDK ABI=$ABI platform=$PLATFORM"

if [[ -f "$OUT/include/shaderc/shaderc.h" &&
      -f "$OUT/libs/c++_static/$ABI/libshaderc.a" ]]; then
    echo "shaderc dist already present: $OUT"
    exit 0
fi

WORK="${RUNNER_TEMP:-$OUT/../shaderc-work}/shaderc"
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

git clone --filter=blob:none https://github.com/google/shaderc.git .
git checkout "$REV"
echo "shaderc $(git describe --always), deps pinned by DEPS:"

# Only the deps libshaderc links: glslang, SPIRV-Tools and SPIRV-Headers. The
# other DEPS entries (abseil, effcee, re2, googletest) serve spirv-diff and the
# tests this build skips; cloning them would cost hundreds of megabytes for
# nothing. GitHub serves any existing SHA to fetch, so a pinned shallow fetch
# is deterministic without cloning full history.
sync() { # <dir under third_party/> <url> <sha>
    echo "  $1 @ $3"
    mkdir -p "third_party/$1"
    git init -q "third_party/$1"
    git -C "third_party/$1" remote add origin "$2"
    git -C "third_party/$1" fetch -q --depth 1 origin "$3"
    git -C "third_party/$1" checkout -q FETCH_HEAD
}
GLSLANG=$(sed -n "s/^  'glslang_revision': '\([0-9a-f]*\)',$/\1/p" DEPS)
SPIRV_TOOLS=$(sed -n "s/^  'spirv_tools_revision': '\([0-9a-f]*\)',$/\1/p" DEPS)
SPIRV_HEADERS=$(sed -n "s/^  'spirv_headers_revision': '\([0-9a-f]*\)',$/\1/p" DEPS)
: "${GLSLANG:?glslang_revision not found in DEPS}"
: "${SPIRV_TOOLS:?spirv_tools_revision not found in DEPS}"
: "${SPIRV_HEADERS:?spirv_headers_revision not found in DEPS}"
sync glslang https://github.com/KhronosGroup/glslang.git "$GLSLANG"
sync spirv-tools https://github.com/KhronosGroup/SPIRV-Tools.git "$SPIRV_TOOLS"
sync spirv-headers https://github.com/KhronosGroup/SPIRV-Headers.git "$SPIRV_HEADERS"

LAUNCHER=()
if command -v ccache >/dev/null 2>&1; then
    LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
# Prefer the cmake the workflow installs through the SDK manager (the runner
# image does not ship one); fall back to whatever is on PATH.
CMAKE_BIN=""
for c in "$ANDROID_HOME"/cmake/*/bin/cmake; do
    if [[ -x "$c" ]]; then CMAKE_BIN="$c"; break; fi
done
[[ -n "$CMAKE_BIN" ]] || CMAKE_BIN="$(command -v cmake || true)"
: "${CMAKE_BIN:?cmake not found on PATH nor under ANDROID_HOME/cmake}"
"$CMAKE_BIN" -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_NDK="$ANDROID_NDK" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$PLATFORM" \
    -DANDROID_STL=c++_static \
    -DSHADERC_SKIP_TESTS=ON \
    -DSHADERC_ENABLE_WERROR_COMPILE=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF \
    -DSPIRV_SKIP_EXECUTABLES=ON \
    -DSPIRV_SKIP_TESTS=ON \
    "${LAUNCHER[@]}"
# shaderc_combined, not shaderc: the plain static target only archives
# shaderc.cc and leaves glslang/SPIRV-Tools as PRIVATE deps, which linking by
# path would never see. The combined target merges every transitive archive
# into one self-contained libshaderc_combined.a (the file the old NDK bundle
# shipped), so the APK's plain-path link keeps working.
"$CMAKE_BIN" --build build --target shaderc_combined

LIB="$(find build -type f -name libshaderc_combined.a -print -quit)"
if [[ -z "$LIB" ]]; then
    echo "libshaderc_combined.a not found under build/" >&2
    exit 1
fi
mkdir -p "$OUT/include" "$OUT/libs/c++_static/$ABI"
cp -r libshaderc/include/. "$OUT/include/"
cp "$LIB" "$OUT/libs/c++_static/$ABI/libshaderc.a"
echo "shaderc installed: $OUT/libs/c++_static/$ABI/libshaderc.a"
