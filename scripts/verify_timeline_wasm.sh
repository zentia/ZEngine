#!/usr/bin/env bash
# Headless verification of the Timeline evaluation core.
#
# Runtime/Function/Timeline/TimelineEvaluation.h is pure standard library, so the test
# needs no engine headers, no shadow tree and no prelude: emcc compiles one TU, node
# runs it. Covers playhead advance (loop remainder, wrap, reverse, finish), which event
# times a tick crosses (each fires exactly once), clip windows, animation phase and
# the length fallback.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

if ! command -v emcc >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source /c/emsdk/emsdk_env.sh >/dev/null 2>&1 || true
fi
command -v emcc >/dev/null 2>&1 || { echo "emcc not found (install emsdk at /c/emsdk)"; exit 2; }

OUT="build-web/timeline_verify"
mkdir -p "${OUT}"

echo "==> compiling TimelineEvaluationTest"
emcc -std=c++23 -O0 -g -Wall -Wextra \
    -I engine/Source \
    engine/Source/Runtime/Function/Timeline/Test/TimelineEvaluationTest.cpp \
    -o "${OUT}/timeline_eval.js"

echo "==> running"
node "${OUT}/timeline_eval.js"
