#!/usr/bin/env bash
# Native (no emcc) run of the editor-side prefab tests + the runtime prefab suite.
#
# verify_prefab_e2e.sh drives PrefabEndToEndSmokeTest through the web build; that needs
# emcc + node. On a machine with neither (macOS dev box, CI image) this runs the same
# tests as real native processes, via scripts/run_smoke_test.py.
#
#   bash scripts/verify_prefab_native.sh            # both suites
#   bash scripts/verify_prefab_native.sh e2e        # PrefabEndToEndSmokeTest only
#   bash scripts/verify_prefab_native.sh types      # same, but with EVERY runtime type registered (P18/types)
#   bash scripts/verify_prefab_native.sh runtime    # ScriptComponentPrefabTest only
#
# WHY SO MANY --extra-source / --exclude-archive-member PAIRS
# bin/libZRuntime.a is the output of the LAST cmake build, which on a dev box is often
# days old. Anything edited since is stale in the archive, and anything added since is
# missing from it. Each source listed here is compiled fresh AND its stale member is
# dropped from the link (otherwise: duplicate symbols). If a new failure looks like
# "the code is right but the test sees old behaviour", the file is probably missing from
# this list — add it to BOTH places.
#
# Native vs web matters: wasm does not trap on a null dereference (low linear memory is
# addressable), so P18's MeshRenderer path wrote through a null RenderSystem and passed
# on web for months. A native run is the only place that class of bug shows up.
#
# Requires (not produced here): bin/libZRuntime.a and build/Generated/ZHT/ZRuntime/Src
# (ZHeaderTool output). After editing a ZPROPERTY / ZCLASS, regenerate with ZHeaderTool
# first — see doc/PrefabSystem_Design.md §9.4.1 "native verification".
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

WHICH="${1:-all}"
G=build/Generated/ZHT/ZRuntime/Src
status=0

# Run in a scratch cwd: the E2E test writes .zasset / .scene / cooked/ into the
# current directory and removes them itself, but a crash would leave them behind.
# Shared by e2e and types: the hand-maintained part of the E2E link line.
e2e_args() {
    E2E_ARGS=(
      --template Function/Audio/AudioSystem.cpp
      --extra-source engine/Source/Editor/Prefab/PrefabInstance.cpp
      --extra-source engine/Source/Editor/Prefab/PrefabUtilityEditor.cpp
      --extra-source engine/Source/Editor/Prefab/PrefabOverrideModel.cpp
      --extra-source engine/Source/Editor/Prefab/PrefabSceneBinding.cpp
      --extra-source engine/Source/Editor/Build/SceneCook.cpp
      --extra-source engine/Source/Editor/Build/BuildDependencyWalk.cpp
      --extra-source engine/Source/Editor/AssetRegistry/AssetRegistry.cpp
      --extra-source engine/Source/Editor/Undo/UndoSystem.cpp
      --extra-source engine/Source/Runtime/Resource/Prefab/PrefabUtility.cpp
      --extra-source engine/Source/Runtime/Resource/Prefab/PrefabSubtreeCloner.cpp
      --extra-source engine/Source/Runtime/Resource/Prefab/PropertyModification.cpp
      --extra-source engine/Source/Runtime/Resource/Prefab/PropertyPathResolver.cpp
      --extra-source engine/Source/Runtime/Resource/Prefab/PropertyValueLocator.cpp
      --extra-source engine/Source/Runtime/Core/YamlSerialize/YamlObjectGraph.cpp
      --extra-source engine/Source/Runtime/Resource/Asset/AssetManager.cpp
      --extra-source engine/Source/Runtime/Resource/Asset/RuntimeAssetManager.cpp
      --extra-source engine/Source/Runtime/BaseClasses/ImmediatePtr.cpp
      --extra-source engine/Source/Runtime/BaseClasses/IPPtrResolver.cpp
      --extra-source engine/Source/Runtime/BaseClasses/GameObject.cpp
      --extra-source engine/Source/Runtime/BaseClasses/TypeManager.cpp
      --extra-source engine/Source/Runtime/BaseClasses/Object.cpp
      --extra-source engine/Source/Runtime/BaseClasses/ObjectManager.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Component/Transform/Transform.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Level/Level.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Component/Mesh/BaseRenderer.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Component/Mesh/MeshRenderer.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Component/Mesh/MaterialCustomParamNames.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Component/Camera/CameraComponent.cpp
      --extra-source engine/Source/Runtime/Function/Framework/Component/Particle/ParticleComponent.cpp
      --extra-source "${G}/GameObject.gen.cpp"
      --extra-source "${G}/Transform.gen.cpp"
      --extra-source "${G}/Behaviour.gen.cpp"
      --extra-source "${G}/PrefabAsset.gen.cpp"
      --extra-source "${G}/PrefabRefComponent.gen.cpp"
      --extra-source "${G}/RigidbodyComponent.gen.cpp"
      --extra-source "${G}/Level.gen.cpp"
      --extra-source "${G}/MeshRenderer.gen.cpp"
      --extra-source "${G}/BaseRenderer.gen.cpp"
      --exclude-archive-member YamlObjectGraph.cpp.o --exclude-archive-member AssetManager.cpp.o
      --exclude-archive-member RuntimeAssetManager.cpp.o --exclude-archive-member ImmediatePtr.cpp.o
      --exclude-archive-member IPPtrResolver.cpp.o --exclude-archive-member Transform.cpp.o
      --exclude-archive-member GameObject.cpp.o --exclude-archive-member TypeManager.cpp.o
      --exclude-archive-member Object.cpp.o --exclude-archive-member ObjectManager.cpp.o
      --exclude-archive-member PrefabUtility.cpp.o --exclude-archive-member Level.cpp.o
      --exclude-archive-member Level.gen.cpp.o --exclude-archive-member BaseRenderer.cpp.o
      --exclude-archive-member MeshRenderer.cpp.o --exclude-archive-member MeshRenderer.gen.cpp.o
      --exclude-archive-member BaseRenderer.gen.cpp.o
      --exclude-archive-member CameraComponent.cpp.o --exclude-archive-member ParticleComponent.cpp.o
    )
}

# `types` mode: add every gen.cpp and every stale class source, computed by
# native_full_link_args.py, minus whatever e2e_args already lists (a source compiled
# twice is a duplicate-symbol link error). See that script for why both are needed.
add_full_type_args() {
    local line kind
    local -a pending=()
    while IFS= read -r line; do pending+=("${line}"); done < <(python3 scripts/native_full_link_args.py)
    local i=0
    while [ "${i}" -lt "${#pending[@]}" ]; do
        kind="${pending[$i]}"; local value="${pending[$((i+1))]}"; i=$((i+2))
        local present=0 a
        for a in "${E2E_ARGS[@]}"; do [ "${a}" = "${value}" ] && present=1; done
        # --extra-source values are paths; the same file may be spelled via ${G}. Compare basenames.
        if [ "${present}" -eq 0 ] && [ "${kind}" = "--extra-source" ]; then
            for a in "${E2E_ARGS[@]}"; do [ "$(basename -- "${a}")" = "$(basename -- "${value}")" ] && present=1; done
        fi
        [ "${present}" -eq 0 ] && E2E_ARGS+=("${kind}" "${value}")
    done
}

run_e2e() {
    echo "==> PrefabEndToEndSmokeTest (native${1:+, all runtime types})"
    local out
    e2e_args
    [ "${1:-}" = "types" ] && add_full_type_args
    out="$(python3 scripts/run_smoke_test.py engine/Source/Editor/Prefab/Test/PrefabEndToEndSmokeTest.cpp "${E2E_ARGS[@]}" 2>&1)"
    echo "${out}" | grep -E "\[FAIL\]|checks,|types round|whole-stream|mismatch|asymmetric|misaligned|error:|Undefined symbols|dyld|FAILED|ALL PASSED" || echo "${out}" | tail -20
    local real_failures floor_only
    real_failures="$(echo "${out}" | grep -c '\[FAIL\]' || true)"
    # P18/types wants >20 registered types. A plain link only has ~9 (see
    # native_full_link_args.py); only the `types` run is expected to meet the floor.
    floor_only=0
    if [ "${1:-}" != "types" ]; then
        floor_only="$(echo "${out}" | grep -c 'P18/types: expected to round-trip' || true)"
    fi
    if [ "${real_failures}" -gt "${floor_only}" ]; then status=1; fi
    if ! echo "${out}" | grep -q "checks,"; then echo "(no result line — the test crashed or did not link)"; status=1; fi
    # In `types` mode every runtime type is registered, so a skipped P21 means the link line
    # lost Camera / Particle -- and the null-system regression guard would silently vanish.
    if [ "${1:-}" = "types" ] && echo "${out}" | grep -q "P21 skipped"; then
        echo "(P21 was skipped in types mode: CameraComponent / ParticleComponent are not registered)"; status=1
    fi
}

run_runtime() {
    echo "==> ScriptComponentPrefabTest (native)"
    local out
    out="$(python3 scripts/run_smoke_test.py engine/Source/Runtime/Scripting/ScriptVM/Test/ScriptComponentPrefabTest.cpp \
      --template Function/Audio/AudioSystem.cpp \
      --extra-source engine/Source/Runtime/Scripting/Backends/QuickJS/BackendQuickJS.cpp \
      --extra-source engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp \
      --extra-source engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp \
      --extra-source "${G}/KeyCodes.gen.cpp" \
      --extra-source engine/Source/Runtime/Core/Time/TimeState.cpp \
      --extra-source engine/Source/Runtime/BaseClasses/TypeManager.cpp \
      --extra-source engine/Source/Runtime/BaseClasses/GameObject.cpp \
      --extra-source engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp \
      --extra-source "${G}/GameObject.gen.cpp" \
      --extra-source "${G}/Transform.gen.cpp" \
      --extra-source "${G}/Behaviour.gen.cpp" \
      --extra-source "${G}/TypeScriptComponent.gen.cpp" \
      --extra-source engine/Source/Runtime/Resource/Prefab/PrefabUtility.cpp \
      --extra-source engine/Source/Runtime/Resource/Prefab/PrefabSubtreeCloner.cpp \
      --extra-source engine/Source/Runtime/Core/YamlSerialize/YamlObjectGraph.cpp \
      --extra-source engine/Source/Runtime/Resource/Asset/AssetManager.cpp \
      --extra-source engine/Source/Runtime/BaseClasses/ImmediatePtr.cpp \
      --extra-source engine/Source/Runtime/BaseClasses/IPPtrResolver.cpp \
      --extra-source engine/Source/Runtime/Function/Framework/Component/Transform/Transform.cpp \
      --extra-source "${G}/PrefabAsset.gen.cpp" \
      --extra-source "${G}/PrefabRefComponent.gen.cpp" \
      --extra-source "${G}/RigidbodyComponent.gen.cpp" \
      --extra-source engine/Source/Runtime/Resource/Prefab/PropertyModification.cpp \
      --extra-source engine/Source/Runtime/Resource/Prefab/PropertyPathResolver.cpp \
      --extra-source engine/Source/Runtime/Resource/Prefab/PropertyValueLocator.cpp \
      --exclude-archive-member YamlObjectGraph.cpp.o --exclude-archive-member AssetManager.cpp.o \
      --exclude-archive-member ImmediatePtr.cpp.o --exclude-archive-member IPPtrResolver.cpp.o \
      --exclude-archive-member Transform.cpp.o \
      --run-arg=--resource-dir --run-arg="engine/Source/Runtime/Scripting/ScriptVM/Test" 2>&1)"
    echo "${out}" | grep -E "\[FAIL\]|checks=|error:|Undefined symbols|RESULT" || echo "${out}" | tail -20
    if ! echo "${out}" | grep -q "RESULT: ALL PASSED"; then status=1; fi
}

case "${WHICH}" in
    e2e)     run_e2e ;;
    types)   run_e2e types ;;
    runtime) run_runtime ;;
    all)     run_runtime; run_e2e; run_e2e types ;;
    *) echo "usage: $0 [all|e2e|types|runtime]"; exit 2 ;;
esac

if [ "${status}" -eq 0 ]; then echo "verify_prefab_native: PASS"; else echo "verify_prefab_native: FAIL"; fi
exit "${status}"
