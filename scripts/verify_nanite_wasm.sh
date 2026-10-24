#!/usr/bin/env bash
# =============================================================================
# verify_nanite_wasm.sh — headless WASM verification for the Nanite cluster
# builder (no RHI, no window, no browser, no reboot).
#
# WHY THIS EXISTS: the Nanite smoke test links the whole ZRuntime, which (since
# the WebGL2 backend landed in #12) drags in WebGL2RHI.o (browser GL symbols) and
# an emmalloc-flavored mimalloc. Resolving those needs -sENVIRONMENT=web, which
# node cannot run, and a real browser is blocked by the sandbox. So the CMake
# `NaniteClusterBuilderSmokeTest` target links fine but can only *run* in a
# browser. This script sidesteps that by compiling the cluster builder + smoke
# test as a standalone TU (they touch no RHI at runtime) and running the result
# under node — a fully autonomous, headless verification.
#
# The standalone TU needs a small prelude (scripts/nanite_wasm_prelude.h) that
# satisfies the serialization codegen headers the full CMake build generates but
# a bare TU does not (DECLARE_SERIALIZE macro + no-op
# INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL + stub transfer types). The Nanite
# smoke test never serializes, so this is sufficient for a header-only parse.
#
# USAGE (from repo root, e.g. E:/Engine/ZEngine):
#   bash scripts/verify_nanite_wasm.sh            # build + run, report pass/fail
#
# REQUIRES: emsdk at C:/emsdk (source emsdk_env.sh), CMake on PATH (not needed
# here — we drive emcc directly), node on PATH.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

# Each emcc shell needs the emsdk env.
source /c/emsdk/emsdk_env.sh >/dev/null 2>&1

PRELUDE="${REPO_ROOT}/scripts/nanite_wasm_prelude.h"
WORKDIR="build-web/nanite_standalone"
mkdir -p "${WORKDIR}"

INC="-I engine/Source -I engine/Source/Runtime -I engine/Source/CommonPCH \
-I engine/3rdparty/bqlog/include -I engine/3rdparty/mimalloc/include \
-I engine/3rdparty/rapidjson/include -I engine/3rdparty/xxHash \
-I engine/3rdparty/puerts/unity/native/EASTL/include"

SRCS=(
  engine/Source/Runtime/Function/Render/Nanite/NaniteCluster.cpp
  engine/Source/Runtime/Function/Render/Nanite/Test/NaniteClusterBuilderSmokeTest.cpp
  engine/Source/Runtime/Core/Math/AxisAligned.cpp
  engine/Source/Runtime/Core/Math/Vector3.cpp
)

echo "==> [build] emcc standalone Nanite cluster builder + smoke test (WASM)"
emcc -std=c++23 -g -O0 -DEASTL_OPENSOURCE=1 \
  -include "${PRELUDE}" ${INC} \
  "${SRCS[@]}" \
  -o "${WORKDIR}/nanite_smoke.js" \
  -s ASSERTIONS=1

echo "==> [run] node ${WORKDIR}/nanite_smoke.js"
node "${WORKDIR}/nanite_smoke.js"
rc=$?

if [ ${rc} -eq 0 ]; then
  echo "==> PASS: Nanite cluster builder smoke test (Raw + Quantized codec) green"
else
  echo "==> FAIL (exit ${rc}): Nanite cluster builder smoke test" >&2
  exit ${rc}
fi
