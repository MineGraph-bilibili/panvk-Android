#!/bin/sh
# Build the optional pinned BCn Vulkan layer. It is never added to the base ICD package.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
REPO=https://github.com/leegao/bcn_layer
COMMIT=50993a2d51772567de9c36de4d523652773f0899
SRC="$ROOT/work/bcn_layer"
BUILD="$ROOT/build/bcn-layer-android"
PATCH="$ROOT/patches/bcn-layer/001-android-layer-enumeration.patch"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
CMAKE="${CMAKE:-$(command -v cmake 2>/dev/null || true)}"
SLANG_VERSION=2026.10.2
SLANG_ARCHIVE="slang-$SLANG_VERSION-linux-x86_64.tar.gz"
SLANG_SHA256=e1993c89fe6eb9fce148672ac2b633af1578e413c41b730be446dd8be251376b
SLANG_ROOT="$ROOT/work/toolchains/slang-$SLANG_VERSION"

[ -n "$NDK" ] || { echo "ANDROID_NDK_HOME or ANDROID_NDK_ROOT is required" >&2; exit 2; }
[ -n "$CMAKE" ] || CMAKE="$(dirname "$(dirname "$NDK")")/cmake/3.22.1/bin/cmake"
[ -x "$CMAKE" ] || { echo "cmake is required (set CMAKE if it is outside PATH)" >&2; exit 2; }
command -v glslc >/dev/null || { echo "glslc is required" >&2; exit 2; }
command -v xxd >/dev/null || { echo "xxd is required" >&2; exit 2; }
command -v curl >/dev/null || { echo "curl is required" >&2; exit 2; }

if [ ! -x "$SLANG_ROOT/bin/slangc" ]; then
  mkdir -p "$SLANG_ROOT"
  curl -fL "https://github.com/shader-slang/slang/releases/download/v$SLANG_VERSION/$SLANG_ARCHIVE" \
    -o "$SLANG_ROOT/$SLANG_ARCHIVE"
  echo "$SLANG_SHA256  $SLANG_ROOT/$SLANG_ARCHIVE" | sha256sum -c -
  tar -xzf "$SLANG_ROOT/$SLANG_ARCHIVE" -C "$SLANG_ROOT"
fi

if [ ! -d "$SRC/.git" ]; then
  git clone --filter=blob:none "$REPO" "$SRC"
fi
git -C "$SRC" fetch origin "$COMMIT"
git -C "$SRC" checkout --detach "$COMMIT"
git -C "$SRC" submodule update --init --recursive
[ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ]
if ! git -C "$SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
  git -C "$SRC" diff --quiet || { echo "bcn_layer checkout has unrelated changes" >&2; exit 2; }
  git -C "$SRC" apply --check "$PATCH"
  git -C "$SRC" apply "$PATCH"
fi

rm -rf "$BUILD"
PATH="$SLANG_ROOT/bin:$PATH" LD_LIBRARY_PATH="$SLANG_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
"$CMAKE" -S "$SRC" -B "$BUILD" \
  -G Ninja \
  -DCMAKE_SYSTEM_NAME=Android \
  -DANDROID_PLATFORM=26 \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_TOOLCHAIN=clang \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -DFORCE_GLSL=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DCMAKE_WARN_DEPRECATED=OFF
PATH="$SLANG_ROOT/bin:$PATH" LD_LIBRARY_PATH="$SLANG_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
"$CMAKE" --build "$BUILD" --parallel
[ -f "$BUILD/libbcn_layer.so" ] || { echo "BUILD-FAIL: libbcn_layer.so absent" >&2; exit 1; }
echo "EXPERIMENTAL-ONLY: $BUILD/libbcn_layer.so"
