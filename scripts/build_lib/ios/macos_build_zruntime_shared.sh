#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CONFIG="Release"
PLATFORM="OS64"
DEPLOYMENT_TARGET="16.0"
CLEAN=0
BUILD_ALL=0
STRIP_OUTPUT="auto"
LIMIT_EXPORTS=1
BUILD_FRAMEWORK=1
FRAMEWORK_NAME="ZRuntimeShared"
FRAMEWORK_BUNDLE_ID="com.zengine.runtime"
PARALLEL_JOBS="${PARALLEL_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
IOS_TOOLCHAIN="$ROOT_DIR/engine/3rdparty/bqlog/build/lib/ios/ios.toolchain.cmake"
# PAPI scripting backend (quickjs | v8). ZEngine is JS-only.
# Defaults to quickjs: it's the only iOS-App-Store-safe option (no JIT) and
# is also what the engine ships on Web/wasm. v8 is selectable for dev-only
# iOS builds (must run in jitless mode under Apple's policy).
PAPI_TYPE="quickjs"

usage() {
  echo "Usage: $(basename "$0") [--config Release|Debug|MinSizeRel|RelWithDebInfo] [--all-configs] [--platform OS64] [--deployment-target 16.0] [--jobs N] [--vulkan-sdk PATH] [--toolchain PATH] [--papi quickjs|v8] [--clean] [--strip] [--no-strip] [--no-export-list] [--framework] [--no-framework] [--framework-name NAME] [--bundle-id ID]"
  echo "Output:    bin/ios/PLATFORM/CONFIG/libZRuntimeShared.dylib"
  echo "Framework: bin/ios/PLATFORM/CONFIG/ZRuntimeShared.framework"
  echo "Symbols:   bin/ios/PLATFORM/CONFIG/libZRuntimeShared.dylib.dSYM"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --all-configs) BUILD_ALL=1; shift ;;
    --platform) PLATFORM="$2"; shift 2 ;;
    --deployment-target) DEPLOYMENT_TARGET="$2"; shift 2 ;;
    --jobs) PARALLEL_JOBS="$2"; shift 2 ;;
    --vulkan-sdk) export VULKAN_SDK="$2"; shift 2 ;;
    --toolchain) IOS_TOOLCHAIN="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --strip) STRIP_OUTPUT=1; shift ;;
    --no-strip) STRIP_OUTPUT=0; shift ;;
    --no-export-list) LIMIT_EXPORTS=0; shift ;;
    --framework) BUILD_FRAMEWORK=1; shift ;;
    --no-framework) BUILD_FRAMEWORK=0; shift ;;
    --framework-name) FRAMEWORK_NAME="$2"; shift 2 ;;
    --bundle-id) FRAMEWORK_BUNDLE_ID="$2"; shift 2 ;;
    --papi) PAPI_TYPE="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1"; usage; exit 1 ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[ERROR] iOS build requires macOS/Xcode."
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[ERROR] cmake not found."
  exit 1
fi

if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "[ERROR] xcodebuild not found. Install/select Xcode first."
  exit 1
fi

if [[ ! -f "$IOS_TOOLCHAIN" ]]; then
  echo "[ERROR] iOS toolchain not found: $IOS_TOOLCHAIN"
  exit 1
fi

if [[ -z "${VULKAN_SDK:-}" ]]; then
  if [[ -d "$HOME/VulkanSDK/local" ]]; then
    export VULKAN_SDK="$HOME/VulkanSDK/local"
  elif [[ -d "$ROOT_DIR/build/vulkan-sdk" ]]; then
    export VULKAN_SDK="$ROOT_DIR/build/vulkan-sdk"
  fi
fi

if [[ -z "${VULKAN_SDK:-}" ]]; then
  echo "[ERROR] VULKAN_SDK is not defined. Use --vulkan-sdk PATH or set VULKAN_SDK."
  exit 1
fi

if [[ ! -d "$VULKAN_SDK" ]]; then
  echo "[ERROR] VULKAN_SDK path does not exist: $VULKAN_SDK"
  exit 1
fi

if [[ ! -x "$VULKAN_SDK/bin/glslangValidator" ]]; then
  echo "[ERROR] glslangValidator not found or not executable: $VULKAN_SDK/bin/glslangValidator"
  exit 1
fi

case "$PLATFORM" in
  OS64|SIMULATOR64|SIMULATORARM64|SIMULATOR64COMBINED) ;;
  *) echo "[WARNING] Uncommon iOS PLATFORM: $PLATFORM" ;;
esac

BUILD_DIR="$ROOT_DIR/build/lib/ios/$PLATFORM"
OUTPUT_SUBDIR="ios/$PLATFORM"
EXPORTS_FILE="$BUILD_DIR/ZRuntimeShared.exports"
case "$PLATFORM" in
  SIMULATOR64|SIMULATORARM64|SIMULATOR64COMBINED)
    FRAMEWORK_SUPPORTED_PLATFORM="iPhoneSimulator"
    FRAMEWORK_PLATFORM_NAME="iphonesimulator"
    ;;
  *)
    FRAMEWORK_SUPPORTED_PLATFORM="iPhoneOS"
    FRAMEWORK_PLATFORM_NAME="iphoneos"
    ;;
esac
CONFIGS=("$CONFIG")
if [[ "$BUILD_ALL" == "1" ]]; then
  CONFIGS=(Debug MinSizeRel Release RelWithDebInfo)
fi

case "$PAPI_TYPE" in
  quickjs|v8) ;;
  *) echo "[ERROR] Unsupported --papi value: $PAPI_TYPE (expected: quickjs|v8)"; exit 1 ;;
esac

# Backend-exclusive C-entrypoint symbols declared in PuertsNative.h: only the
# ones for the active PAPI backend will be implemented (by libPapi<Backend>.a),
# so we must NOT request the linker to export the others or `-exported_symbols_list`
# will fail with "Undefined symbols ... <initial-undefines>".
case "$PAPI_TYPE" in
  quickjs) PAPI_KEEP_BACKEND="Qjs" ;;
  v8)      PAPI_KEEP_BACKEND="V8" ;;
esac

cat <<EOF
========================================
ZRuntimeShared iOS Build
========================================
Root:       $ROOT_DIR
Toolchain:  $IOS_TOOLCHAIN
Vulkan SDK: $VULKAN_SDK
Platform:   $PLATFORM
Deploy:     $DEPLOYMENT_TARGET
PAPI:       $PAPI_TYPE
Configs:    ${CONFIGS[*]}
Strip:      $STRIP_OUTPUT (auto strips Release/MinSizeRel)
Output:     $ROOT_DIR/bin/$OUTPUT_SUBDIR/<CONFIG>/libZRuntimeShared.dylib
Framework:  $ROOT_DIR/bin/$OUTPUT_SUBDIR/<CONFIG>/$FRAMEWORK_NAME.framework (enabled=$BUILD_FRAMEWORK)
Symbols:    $ROOT_DIR/bin/$OUTPUT_SUBDIR/<CONFIG>/libZRuntimeShared.dylib.dSYM
EOF

if [[ "$CLEAN" == "1" ]]; then
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

if [[ "$LIMIT_EXPORTS" == "1" ]]; then
  {
    awk -v keep="$PAPI_KEEP_BACKEND" '/EXPORT_RUNTIME/ {
      line=$0
      if (line ~ /^[[:space:]]*#/) next
      sub(/\(.*/, "", line)
      n=split(line, parts, /[^A-Za-z0-9_]+/)
      if (n > 0 && parts[n] != "") {
        sym=parts[n]
        # Drop backend-exclusive entries that are not implemented by the
        # currently selected PAPI backend (otherwise the linker will fail
        # with "Undefined symbols ... <initial-undefines>").
        # Symbols look like: GetQjsPapiVersion / CreateV8PapiEnvRef / ...
        # Lua/Python/Nodejs are kept in the regex even though those backends
        # are no longer built -- in case puerts upstream still exports the
        # decls, we filter them out anyway since `keep` will never match.
        if (sym ~ /^(Get|Create|Destroy)(Lua|Qjs|Python|Nodejs|V8)(FFIApi|PapiVersion|PapiEnvRef)$/) {
          if (sym !~ ("(Lua|Qjs|Python|Nodejs|V8)") || sym !~ keep) next
        }
        print "_" sym
      }
    }' "$ROOT_DIR/engine/source/Runtime/Scripting/Native/PuertsNative.h"
    awk '/ZENGINE_API_EXPORT/ {
      line=$0
      if (line ~ /^[[:space:]]*#/) next
      sub(/\(.*/, "", line)
      n=split(line, parts, /[^A-Za-z0-9_]+/)
      if (n > 0 && parts[n] != "") print "_" parts[n]
    }' "$ROOT_DIR/engine/source/Runtime/ZEngineApi/ZEngineApi.cpp"
  } | sort -u > "$EXPORTS_FILE"
fi

CMAKE_EXPORT_ARGS=()
if [[ "$LIMIT_EXPORTS" == "1" ]]; then
  CMAKE_EXPORT_ARGS=(-DZENGINE_IOS_EXPORT_SYMBOLS_FILE="$EXPORTS_FILE")
fi

create_framework() {
  local dylib_path="$1"
  local build_config="$2"
  local output_dir="$ROOT_DIR/bin/$OUTPUT_SUBDIR/$build_config"
  local framework_dir="$output_dir/$FRAMEWORK_NAME.framework"
  local framework_binary="$framework_dir/$FRAMEWORK_NAME"
  local dsym_path="$dylib_path.dSYM"
  local framework_dsym="$framework_dir.dSYM"

  rm -rf "$framework_dir"
  mkdir -p "$framework_dir/Headers" "$framework_dir/Modules"

  cp -f "$dylib_path" "$framework_binary"
  chmod 755 "$framework_binary"

  if command -v install_name_tool >/dev/null 2>&1; then
    install_name_tool -id "@rpath/$FRAMEWORK_NAME.framework/$FRAMEWORK_NAME" "$framework_binary" || \
      echo "[WARNING] Failed to update framework install name: $framework_binary"
  fi

  cat > "$framework_dir/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>$FRAMEWORK_NAME</string>
  <key>CFBundleIdentifier</key>
  <string>$FRAMEWORK_BUNDLE_ID</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>$FRAMEWORK_NAME</string>
  <key>CFBundlePackageType</key>
  <string>FMWK</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>MinimumOSVersion</key>
  <string>$DEPLOYMENT_TARGET</string>
  <key>DTPlatformName</key>
  <string>$FRAMEWORK_PLATFORM_NAME</string>
  <key>CFBundleSupportedPlatforms</key>
  <array>
    <string>$FRAMEWORK_SUPPORTED_PLATFORM</string>
  </array>
</dict>
</plist>
PLIST

  for header in \
    "$ROOT_DIR/engine/Source/Runtime/export_runtime.h" \
    "$ROOT_DIR/engine/Source/Runtime/Scripting/Native/PuertsNative.h" \
    "$ROOT_DIR/engine/Source/Runtime/ZEngineApi/ZEngineApi.h" \
    "$ROOT_DIR/engine/Source/Runtime/ZEngineApi/ZEngineApi.export.h"; do
    if [[ -f "$header" ]]; then
      cp -f "$header" "$framework_dir/Headers/"
    fi
  done

  cat > "$framework_dir/Headers/$FRAMEWORK_NAME.h" <<HEADER
#pragma once

#if __has_include(<$FRAMEWORK_NAME/export_runtime.h>)
#include <$FRAMEWORK_NAME/export_runtime.h>
#endif
#if __has_include(<$FRAMEWORK_NAME/PuertsNative.h>)
#include <$FRAMEWORK_NAME/PuertsNative.h>
#endif
#if __has_include(<$FRAMEWORK_NAME/ZEngineApi.h>)
#include <$FRAMEWORK_NAME/ZEngineApi.h>
#endif
HEADER

  cat > "$framework_dir/Modules/module.modulemap" <<MODULEMAP
framework module $FRAMEWORK_NAME {
  umbrella header "$FRAMEWORK_NAME.h"
  export *
  module * { export * }
}
MODULEMAP

  if [[ -d "$dsym_path" ]]; then
    rm -rf "$framework_dsym"
    cp -R "$dsym_path" "$framework_dsym"
    local dwarf_dir="$framework_dsym/Contents/Resources/DWARF"
    if [[ -f "$dwarf_dir/libZRuntimeShared.dylib" ]]; then
      mv "$dwarf_dir/libZRuntimeShared.dylib" "$dwarf_dir/$FRAMEWORK_NAME"
    fi
  fi

  if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - "$framework_dir" >/dev/null || \
      echo "[WARNING] Ad-hoc codesign failed: $framework_dir"
  fi

  echo "[FRAMEWORK] $framework_dir"
  if [[ -d "$framework_dsym" ]]; then
    echo "[FRAMEWORK SYMBOLS] $framework_dsym"
  fi
}

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE="$IOS_TOOLCHAIN" \
  -DPLATFORM="$PLATFORM" \
  -DDEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DENABLE_ARC=OFF \
  -DTARGET_PLATFORM:STRING=ios \
  -DPAPI_TYPE="$PAPI_TYPE" \
  -DJPH_USE_VK=OFF \
  -DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=OFF \
  -DCMAKE_XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING=YES \
  "${CMAKE_EXPORT_ARGS[@]}" \
  -DZENGINE_OUTPUT_SUBDIR="$OUTPUT_SUBDIR"

for build_config in "${CONFIGS[@]}"; do
  cmake --build "$BUILD_DIR" --config "$build_config" --target ZRuntimeShared --parallel "$PARALLEL_JOBS"

  out_dylib="$ROOT_DIR/bin/$OUTPUT_SUBDIR/$build_config/libZRuntimeShared.dylib"
  if [[ ! -f "$out_dylib" ]]; then
    echo "[ERROR] Output library not found: $out_dylib"
    exit 1
  fi
  do_strip=0
  if [[ "$STRIP_OUTPUT" == "1" ]]; then
    do_strip=1
  elif [[ "$STRIP_OUTPUT" == "auto" && ( "$build_config" == "Release" || "$build_config" == "MinSizeRel" ) ]]; then
    do_strip=1
  fi

  if [[ "$do_strip" == "1" ]]; then
    xcrun strip -S -x "$out_dylib"
  fi

  if [[ "$BUILD_FRAMEWORK" == "1" ]]; then
    create_framework "$out_dylib" "$build_config"
  fi

  echo "[SUCCESS] $out_dylib"
  if [[ -d "$out_dylib.dSYM" ]]; then
    echo "[SYMBOLS] $out_dylib.dSYM"
  fi
  if [[ "$do_strip" == "1" ]]; then
    echo "[STRIPPED] $out_dylib"
  fi
done
