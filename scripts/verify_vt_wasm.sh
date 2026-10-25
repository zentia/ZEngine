#!/usr/bin/env bash
# =============================================================================
# verify_vt_wasm.sh — headless WASM verification for the Virtual Texture
# streaming POLICY (CPU bookkeeping half), no RHI / no device / no browser.
#
# WHY THIS EXISTS: like the Nanite harness (verify_nanite_wasm.sh), the VT smoke
# test must run without a real Vulkan device. But VT is harder than Nanite: the
# three VirtualTexture *.cpp files hard-#include "VulkanRHI.h", which pulls
# <vulkan/vulkan.h> + <vma/vk_mem_alloc.h> at file scope (unavailable under
# emcc). We shadow just that header (scripts/vt_wasm_shadow/.../VulkanRHI.h) and
# place the shadow root BEFORE engine/Source on the include path. The shadow
# provides exactly the Vk/vma surface the VT pool references; the real
# PhysicalTexturePool::Initialize() Vulkan body is compiled out (Z_HAS_VULKAN
# undefined on Emscripten) and the harness uses InitializeLogical() instead.
#
# The streaming policy exercised here (RequestPages -> Update -> LoadPage ->
# EvictLRUPages -> stats) is pure CPU slot bookkeeping; it works identically on
# the logical (no-GPU) pool. The 3 GPU TODOs (page upload / page-table texture)
# are deliberately NOT covered -- those need a real device.
#
# USAGE (from repo root, e.g. E:/Engine/ZEngine):
#   bash scripts/verify_vt_wasm.sh
#
# REQUIRES: emsdk at C:/emsdk, node on PATH.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

source /c/emsdk/emsdk_env.sh >/dev/null 2>&1

SHADOW="${REPO_ROOT}/scripts/vt_wasm_shadow"
# The VT headers pull the full render scene (RenderScene -> Light -> Vector4),
# whose math types use DECLARE_SERIALIZE + INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL
# (the serialization codegen the bare TU doesn't run). Reuse the Nanite harness
# prelude, which satisfies both with a no-op macro + stub transfer types (the VT
# smoke test never serializes).
PRELUDE="${REPO_ROOT}/scripts/nanite_wasm_prelude.h"
INC="-I ${SHADOW} -I engine/Source -I engine/Source/Runtime -I engine/Source/CommonPCH \
-I engine/3rdparty/bqlog/include -I engine/3rdparty/mimalloc/include \
-I engine/3rdparty/rapidjson/include -I engine/3rdparty/xxHash \
-I engine/3rdparty/puerts/unity/native/EASTL/include"

SRCS=(
  engine/Source/Runtime/Function/Render/Texture/VirtualTextureTypes.cpp
  engine/Source/Runtime/Function/Render/Texture/VirtualTextureResource.cpp
  engine/Source/Runtime/Function/Render/Texture/VirtualTextureManager.cpp
  engine/Source/Runtime/Function/Render/Texture/VirtualTextureSettings.cpp
  engine/Source/Runtime/Function/Render/Texture/Test/VirtualTextureStreamingSmokeTest.cpp
  engine/Source/Runtime/Core/Math/AxisAligned.cpp
  engine/Source/Runtime/Core/Math/Vector3.cpp
  # Shadow TU defining the vmaCreateImage/vmaDestroyImage stubs the pool's
  # non-Apple #else body references (never actually executed by the harness).
  scripts/vt_wasm_shadow/vt_wasm_vma_shim.cpp
)

WORKDIR="build-web/vt_standalone"
mkdir -p "${WORKDIR}"

echo "==> [build] emcc standalone VT streaming policy + smoke test (WASM)"
# The VT headers pull the full render scene (RenderResourceBase -> RenderScene ->
# RenderCamera -> PPtr -> Object -> ObjectDefines -> Macro -> Platform.h), whose
# platform detection #errors on Emscripten (no matching Z_PLATFORM_*). Emscripten
# targets a POSIX-like wasm32 environment, so present the Linux/x64/Clang triple:
# Platform.h then includes <unistd.h> and uses the POSIX path separator, which is
# correct for the wasm runtime. The Nanite harness avoided this by only pulling
# math headers; VT's deeper include chain needs the explicit platform triple.
emcc -std=c++23 -g -O0 -DEASTL_OPENSOURCE=1 \
  -DZ_PLATFORM_LINUX -DZ_ARCH_X64 -DZ_COMPILER_CLANG \
  -include "${PRELUDE}" ${INC} \
  "${SRCS[@]}" \
  -o "${WORKDIR}/vt_smoke.js" \
  -s ASSERTIONS=1

echo "==> [run] node ${WORKDIR}/vt_smoke.js"
node "${WORKDIR}/vt_smoke.js"
rc=$?

if [ ${rc} -eq 0 ]; then
  echo "==> PASS: VirtualTexture streaming policy smoke test green"
else
  echo "==> FAIL (exit ${rc}): VirtualTexture streaming policy smoke test" >&2
  exit ${rc}
fi
