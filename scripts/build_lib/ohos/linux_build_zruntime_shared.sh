#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CONFIG="Release"
ABI="arm64-v8a"
CLEAN=0
STRIP_OUTPUT=1
PARALLEL_JOBS="${PARALLEL_JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 8)}"

usage() {
  echo "Usage: $(basename "$0") [--abi arm64-v8a] [--config Release] [--jobs N] [--ohos-sdk PATH] [--clean] [--no-strip]"
  echo "Output:  bin/ohos/ABI/CONFIG/libZRuntimeShared.so"
  echo "Symbols: bin/ohos/symbols/CONFIG/ABI/libZRuntimeShared.so"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --abi) ABI="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --jobs) PARALLEL_JOBS="$2"; shift 2 ;;
    --ohos-sdk) export OHOS_SDK="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --no-strip) STRIP_OUTPUT=0; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1"; usage; exit 1 ;;
  esac
done

if [[ -z "${OHOS_SDK:-}" ]]; then
  echo "[ERROR] OHOS_SDK is not defined. Set OHOS_SDK or pass --ohos-sdk PATH."
  exit 1
fi

NDK_PATH="$OHOS_SDK/native"
if [[ ! -f "$NDK_PATH/build/cmake/ohos.toolchain.cmake" ]]; then
  echo "[ERROR] OHOS toolchain not found: $NDK_PATH/build/cmake/ohos.toolchain.cmake"
  exit 1
fi
STRIP_EXE="$NDK_PATH/llvm/bin/llvm-strip"

BUILD_DIR="$ROOT_DIR/build/lib/ohos/$ABI/$CONFIG"
OUTPUT_SUBDIR="ohos/$ABI"
OUT_SO="$ROOT_DIR/bin/ohos/$ABI/$CONFIG/libZRuntimeShared.so"
SYMBOL_SO="$ROOT_DIR/bin/ohos/symbols/$CONFIG/$ABI/libZRuntimeShared.so"

cat <<EOF
========================================
ZRuntimeShared OHOS Build
========================================
Root:   $ROOT_DIR
SDK:    $OHOS_SDK
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
  -DCMAKE_TOOLCHAIN_FILE="$NDK_PATH/build/cmake/ohos.toolchain.cmake" \
  -DOHOS_ARCH="$ABI" \
  -DOHOS_PLATFORM=ohos \
  -DOHOS_STL=c++_static \
  -DTARGET_PLATFORM:STRING=ohos \
  -DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=OFF \
  -DGLFW_BUILD_X11=OFF \
  -DGLFW_BUILD_WAYLAND=OFF \
  -DCURL_ENABLE_SSL=OFF \
  -DCURL_USE_OPENSSL=OFF \
  -DZENGINE_MOBILE_SIZE_OPTIMIZE=ON \
  -DZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS=ON \
  -DZENGINE_OUTPUT_SUBDIR="$OUTPUT_SUBDIR"

cmake --build "$BUILD_DIR" --target ZRuntimeShared -- -j"$PARALLEL_JOBS"

if [[ "$STRIP_OUTPUT" == "1" ]]; then
  if [[ ! -x "$STRIP_EXE" ]]; then
    echo "[ERROR] llvm-strip not found: $STRIP_EXE"
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
