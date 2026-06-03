#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CONFIG="Release"
ABI="arm64-v8a"
ANDROID_PLATFORM="android-24"
CLEAN=0
STRIP_OUTPUT=1
PARALLEL_JOBS="${PARALLEL_JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 8)}"

usage() {
  echo "Usage: $(basename "$0") [--abi arm64-v8a] [--config Release] [--platform android-24] [--jobs N] [--ndk PATH] [--clean] [--no-strip]"
  echo "Output:  bin/android/ABI/CONFIG/libZRuntimeShared.so"
  echo "Symbols: bin/android/symbols/CONFIG/ABI/libZRuntimeShared.so"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --abi) ABI="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --platform) ANDROID_PLATFORM="$2"; shift 2 ;;
    --jobs) PARALLEL_JOBS="$2"; shift 2 ;;
    --ndk) export ANDROID_NDK_ROOT="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --no-strip) STRIP_OUTPUT=0; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1"; usage; exit 1 ;;
  esac
done

ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}"
if [[ -z "$ANDROID_NDK_ROOT" || ! -f "$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" ]]; then
  echo "[ERROR] Android toolchain not found. Set ANDROID_NDK_ROOT or pass --ndk PATH."
  exit 1
fi

uname_s="$(uname -s)"
uname_m="$(uname -m)"
case "$uname_s:$uname_m" in
  Linux:aarch64|Linux:arm64) HOST_TAG="linux-arm64" ;;
  Linux:*) HOST_TAG="linux-x86_64" ;;
  Darwin:arm64) HOST_TAG="darwin-arm64" ;;
  Darwin:*) HOST_TAG="darwin-x86_64" ;;
  *) HOST_TAG="linux-x86_64" ;;
esac
if [[ ! -x "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip" ]]; then
  for candidate in "$ANDROID_NDK_ROOT"/toolchains/llvm/prebuilt/*/bin/llvm-strip; do
    [[ -x "$candidate" ]] && STRIP_EXE="$candidate" && break
  done
else
  STRIP_EXE="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip"
fi

BUILD_DIR="$ROOT_DIR/build/lib/android/$ABI/$CONFIG"
OUTPUT_SUBDIR="android/$ABI"
OUT_SO="$ROOT_DIR/bin/android/$ABI/$CONFIG/libZRuntimeShared.so"
SYMBOL_SO="$ROOT_DIR/bin/android/symbols/$CONFIG/$ABI/libZRuntimeShared.so"

cat <<EOF
========================================
ZRuntimeShared Android Build
========================================
Root:   $ROOT_DIR
NDK:    $ANDROID_NDK_ROOT
ABI:    $ABI
Config: $CONFIG
Output: $OUT_SO
Symbols:$SYMBOL_SO
Strip:  $STRIP_OUTPUT
EOF

if [[ "$CLEAN" == "1" ]]; then
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_NDK="$ANDROID_NDK_ROOT" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
  -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
  -DTARGET_PLATFORM:STRING=android \
  -DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=OFF \
  -DZENGINE_MOBILE_SIZE_OPTIMIZE=ON \
  -DZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS=ON \
  -DZENGINE_OUTPUT_SUBDIR="$OUTPUT_SUBDIR"

cmake --build "$BUILD_DIR" --target ZRuntimeShared -- -j"$PARALLEL_JOBS"

if [[ "$STRIP_OUTPUT" == "1" ]]; then
  if [[ -z "${STRIP_EXE:-}" || ! -x "$STRIP_EXE" ]]; then
    echo "[ERROR] llvm-strip not found under Android NDK."
    exit 1
  fi
  mkdir -p "$(dirname "$SYMBOL_SO")"
  cp -f "$OUT_SO" "$SYMBOL_SO"
  "$STRIP_EXE" -s "$OUT_SO"
fi

echo "[SUCCESS] $OUT_SO"
if [[ "$STRIP_OUTPUT" == "1" ]]; then
  echo "[SYMBOLS] $SYMBOL_SO"
fi
