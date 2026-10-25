#!/usr/bin/env bash
# Headless prefab end-to-end verification (doc/PrefabSystem_Design.md §9.4 items 1 + 2).
#
# Builds engine/Source/Editor/Prefab/Test/PrefabEndToEndSmokeTest.cpp plus the
# Editor/Prefab TUs against the web build's libZRuntime.a and runs it under node
# (scripts/web_smoke_run.py does the compile/link/run). Also re-runs the
# serialization smoke tests the prefab fixes touch (Transform::Transfer,
# StoreExternalAssetPath, LocalFileSystemWeb::Size).
#
#   bash scripts/verify_prefab_e2e.sh            # uses bin/libZRuntime.a as is
#   bash scripts/verify_prefab_e2e.sh --rebuild  # brings libZRuntime.a up to date first
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

if ! command -v emcc >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source /c/emsdk/emsdk_env.sh >/dev/null 2>&1 || true
fi

REBUILD=()
[ "${1:-}" = "--rebuild" ] && REBUILD=(--rebuild-lib)

EXTRA=()
for f in engine/Source/Editor/Prefab/*.cpp; do EXTRA+=(--extra "$f"); done

status=0
echo "==> PrefabEndToEndSmokeTest"
python scripts/web_smoke_run.py engine/Source/Editor/Prefab/Test/PrefabEndToEndSmokeTest.cpp \
    "${REBUILD[@]}" "${EXTRA[@]}" || status=1

for t in YamlRoundTripSmokeTest SchemaEvolutionSmokeTest; do
    echo "==> ${t}"
    python scripts/web_smoke_run.py "engine/Source/Runtime/Core/Serialize/Test/${t}.cpp" || status=1
done

if [ "${status}" -eq 0 ]; then
    echo "verify_prefab_e2e: PASS"
else
    echo "verify_prefab_e2e: FAIL"
fi
exit "${status}"
