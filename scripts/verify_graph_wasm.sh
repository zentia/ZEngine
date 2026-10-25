#!/usr/bin/env bash
# =============================================================================
# verify_graph_wasm.sh — headless WASM verification for the RenderGraph
# compiler (RenderGraphSmokeTest) AND executor (RenderGraphExecuteSmokeTest),
# no GPU / no device / no browser.
#
# WHY THIS EXISTS: RenderGraph is what Phase 2 step 2.4 (RenderGraph 接管 RP1)
# builds on. Both halves run headless:
#   * compiler: RenderGraph.cpp includes only <algorithm>/<queue>/<set>/
#     <utility> + its own headers -- pure stdlib.
#   * executor: RenderGraphResources.cpp talks to the 10-method IRGResourceDevice
#     (the test uses a FakeDevice), so it only needs RHI.h/RHIStruct.h for the
#     opaque types -- plus the platform triple so Platform.h does not #error
#     under Emscripten (same triple as verify_vt_wasm.sh).
# No shadow tree, no serialization prelude.
#
# Coverage relevant to step 2.4 (RP1 GBuffer -> DeferredLighting -> Forward):
#   * compiler groups 19-20: order, 9 abstract barriers, transient lifetimes,
#     same-pass read+write rejected.
#   * executor groups 13-14: what actually reaches the RHI -- batched calls,
#     layouts, and the depth ASPECT (regression for the import-format bug).
#
# USAGE (from repo root, e.g. E:/Engine/ZEngine):
#   bash scripts/verify_graph_wasm.sh
#
# REQUIRES: emsdk at C:/emsdk, node on PATH.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

source /c/emsdk/emsdk_env.sh >/dev/null 2>&1

OUT_DIR="${REPO_ROOT}/build-web/graph_verify"
mkdir -p "${OUT_DIR}"

G=engine/Source/Runtime/Function/Render/Graph
CORE_TUS="${G}/RenderGraphTypes.cpp ${G}/RenderGraphBuilder.cpp ${G}/RenderGraph.cpp"

echo "==> [1/2] compiler: RenderGraphSmokeTest"
emcc -std=c++23 -g -O0 ${CORE_TUS} ${G}/Test/RenderGraphSmokeTest.cpp \
  -I engine/Source \
  -o "${OUT_DIR}/graph.js" -s ASSERTIONS=1
node "${OUT_DIR}/graph.js"

echo "==> [2/2] executor: RenderGraphExecuteSmokeTest"
emcc -std=c++23 -g -O0 -DZ_PLATFORM_LINUX -DZ_ARCH_X64 -DZ_COMPILER_CLANG -DEASTL_OPENSOURCE=1 \
  ${CORE_TUS} ${G}/RenderGraphResources.cpp ${G}/Test/RenderGraphExecuteSmokeTest.cpp \
  -I engine/Source -I engine/Source/Runtime -I engine/Source/CommonPCH \
  -I engine/3rdparty/puerts/unity/native/EASTL/include \
  -o "${OUT_DIR}/graph_exec.js" -s ASSERTIONS=1
node "${OUT_DIR}/graph_exec.js"

echo "==> RenderGraph compiler + executor: PASS"
