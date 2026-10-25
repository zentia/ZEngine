#!/usr/bin/env bash
# =============================================================================
# verify_graph_wasm.sh — headless WASM verification for the RenderGraph
# compiler (RenderGraphSmokeTest), no RHI / no device / no browser.
#
# WHY THIS EXISTS: the RenderGraph compiler is the capability Phase 2 step 2.4
# (RenderGraph 接管 RP1) is built on top of. The compiler is pure CPU and
# deterministic (RenderGraph.cpp includes only <algorithm>/<queue>/<set>/
# <utility> + its own headers -- no RHI, no bqlog, no object system), so it
# compiles as a standalone TU set under emcc and runs on node, exactly like the
# Nanite/VT harnesses but with NONE of their shadow-tree machinery.
#
# The smoke test already covers the RP2 post-process chain (groups 14-18) and,
# as of step 2.4, the RP1 three-way-split chain (groups 19-20):
#   GBufferPass -> DeferredLightingPass -> ForwardLightingPass.
# This is the compiler-level proof that the topology RENDER_GRAPH.md:117 flagged
# as "whole-segment only" IS expressible at the graph level. The two real
# semantic gaps (input-attachment vs sampled descriptor; ForwardLighting blend)
# are RHI/execute-layer work, not modeled here -- see PHASE2_PLAN.md 落地记录·2.4.
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

echo "==> compiling RenderGraphSmokeTest as standalone WASM TU"
emcc -std=c++23 -g -O0 \
  engine/Source/Runtime/Function/Render/Graph/RenderGraphTypes.cpp \
  engine/Source/Runtime/Function/Render/Graph/RenderGraphBuilder.cpp \
  engine/Source/Runtime/Function/Render/Graph/RenderGraph.cpp \
  engine/Source/Runtime/Function/Render/Graph/Test/RenderGraphSmokeTest.cpp \
  -I engine/Source \
  -o "${OUT_DIR}/graph.js" \
  -s ASSERTIONS=1

echo "==> running RenderGraphSmokeTest (headless, no device)"
node "${OUT_DIR}/graph.js"
