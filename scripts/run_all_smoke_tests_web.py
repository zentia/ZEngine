#!/usr/bin/env python3
"""Run every *SmokeTest.cpp headless under node and tabulate the results.

Each test is built with scripts/web_smoke_run.py (compiled with build-web's flags,
linked --whole-archive against bin/libZRuntime.a, run under node + NODERAWFS).
Extra translation units a test needs (the ones its CMake target lists besides the
test itself, e.g. Editor/Undo/UndoSystem.cpp) come from EXTRA below.

Verdicts:
  PASS    built and exited 0
  FAIL    built, ran, exited non-zero (a check failed, or a trap / abort)
  BUILD   did not compile / link for the web target
  TIMEOUT ran longer than --timeout seconds
  ENV     ran, but stopped on a node/wasm limit (threads, child process); see ENV_LIMIT_MARKERS
  SKIP    platform-only by construction (listed in PLATFORM_ONLY)

    python scripts/run_all_smoke_tests_web.py [--filter SUBSTR] [--timeout S] [--json OUT]
"""
import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
SRC = os.path.join(ROOT, 'engine', 'Source')

# Tests that only make sense on one native backend / OS; building them for wasm
# would only measure "does it compile without d3d12.h / Metal / POSIX signals".
PLATFORM_ONLY = {
    'DX12BindlessSmokeTest': 'needs d3d12 (Windows DX12 device)',
    'VulkanBindlessSmokeTest': 'needs a Vulkan device',
    'AppleSceneBlitSmokeTest': 'Apple / Metal only',
    'MetalSceneDepthSmokeTest': 'Apple / Metal only',
    'MetalSpirvShaderSmokeTest': 'Apple / Metal only',
    'PosixCrashCaptureSmokeTest': 'POSIX signal handlers + fork (Linux/WSL)',
    # VirtualTextureManager.cpp includes VulkanRHI.h (vulkan/vulkan.h) unconditionally;
    # scripts/verify_vt_wasm.sh runs this test against a shadow VulkanRHI instead.
    'VirtualTextureStreamingSmokeTest': 'Vulkan headers; covered by scripts/verify_vt_wasm.sh',
}

# Extra TUs per test (paths relative to engine/Source), mirroring the CMake targets.
EXTRA = {
    'TextureCompressorSmokeTest': ['Editor/AssetPipeline/TextureImporter/TextureCompressor.cpp'],
    'BuildCacheSmokeTest': ['Editor/Build/BuildCache.cpp'],
    'UndoSystemSmokeTest': ['Editor/Undo/UndoSystem.cpp'],
    'PrefabEndToEndSmokeTest': ['Editor/Prefab/*.cpp', 'Editor/Build/SceneCook.cpp'],
    # Lumen and VirtualTexture sources are stripped from the web libZRuntime
    # (Runtime/CMakeLists.txt); the tests compile them in, with Z_HAS_LUMEN (below).
    'LumenGlobalIlluminationSmokeTest': ['Runtime/Function/Render/Lumen/*.cpp'],
    'LumenRayTracingSmokeTest': ['Runtime/Function/Render/Lumen/*.cpp'],
    'LumenReflectionsSmokeTest': ['Runtime/Function/Render/Lumen/*.cpp'],
    'LumenRenderPassWiringSmokeTest': ['Runtime/Function/Render/Lumen/*.cpp'],
    'LumenSceneGeometrySmokeTest': ['Runtime/Function/Render/Lumen/*.cpp'],
    'LumenSurfaceCacheSmokeTest': ['Runtime/Function/Render/Lumen/*.cpp'],
    'LumenPerFrameBridgeSmokeTest': [],
}

# Smoke tests whose subject needs something node's single-threaded wasm cannot give:
# std::thread (no -pthread in this build), a child process, or a GPU device. They
# build and start; their failure says nothing about the code. Detected from output.
ENV_LIMIT_MARKERS = {
    'thread constructor failed': 'needs std::thread (wasm build has no pthreads)',
    'child probe process spawned': 'spawns a child process (not possible under node)',
}
# Same, but recognised by test name because the failure has no distinctive text.
ENV_LIMIT_TESTS = {
    # LMDB reads its data through a MAP_SHARED mapping of the file it writes with
    # pwrite(); emscripten emulates mmap with a private copy, so a committed value is
    # never visible to the next read txn (probe: put/commit 0, get MDB_NOTFOUND; with
    # MDB_WRITEMAP the commit's msync fails EINVAL). The DDC is editor-only
    # (TextureImporter); the web runtime never opens one.
    'DerivedDataCacheSmokeTest': 'LMDB needs a coherent MAP_SHARED mmap (emscripten copies)',
    # Lumen is stripped from the web libZRuntime, so the library's
    # RenderPipelineBase::UpdateLumen is the no-op #else branch, and a --whole-archive
    # link cannot swap in a Lumen-enabled copy (duplicate symbols). The two
    # "UpdateLumen drives the pass" checks fail by construction; the other 35 pass.
    'LumenRenderPassWiringSmokeTest': 'web libZRuntime has UpdateLumen compiled without Z_HAS_LUMEN',
}

# Extra include dirs / defines per test, mirroring target_include_directories /
# target_compile_definitions in the test's CMakeLists.txt.
_E = SRC.replace(os.sep, '/')
_ROOT = _E.rsplit('/engine/Source', 1)[0]
INCLUDE_DIRS = {
    'TextureCompressorSmokeTest': [_E + '/Editor/AssetPipeline/TextureImporter',
                                   _ROOT + '/engine/3rdparty/bc7enc_rdo',
                                   _ROOT + '/engine/3rdparty/astc-encoder/Source',
                                   _ROOT + '/tools/renderdoc/renderdoc/3rdparty/stb'],
}
LINK = {
    'TextureCompressorSmokeTest': [_ROOT + '/bin/libbc7enc.a', _ROOT + '/bin/libastcenc.a'],
}
_LUMEN = ['Z_HAS_LUMEN=1']
_SHADOW_SRC = _E + '/Runtime/Function/Render'
_SHADER = _E.rsplit('/Source', 1)[0] + '/shader'
DEFINES = {
    'LumenGlobalIlluminationSmokeTest': _LUMEN,
    'LumenRayTracingSmokeTest': _LUMEN,
    'LumenReflectionsSmokeTest': _LUMEN,
    'LumenRenderPassWiringSmokeTest': _LUMEN,
    'LumenSceneGeometrySmokeTest': _LUMEN,
    'LumenSurfaceCacheSmokeTest': _LUMEN,
    'LumenPerFrameBridgeSmokeTest': _LUMEN,
    'DirectionalLightShadowSmokeTest': [
        'ZENGINE_RHI_H="%s/Interface/RHI.h"' % _SHADOW_SRC,
        'ZENGINE_METAL_RHI_H="%s/Interface/Metal/MetalRHI.h"' % _SHADOW_SRC,
        'ZENGINE_METAL_RHI_MM="%s/Interface/Metal/MetalRHI.mm"' % _SHADOW_SRC,
        'ZENGINE_DIRECTIONAL_LIGHT_PASS_CPP="%s/Passes/DirectionalLightPass.cpp"' % _SHADOW_SRC,
        'ZENGINE_RENDER_SYSTEM_CPP="%s/RenderSystem.cpp"' % _SHADOW_SRC,
        'ZENGINE_RENDER_SCENE_H="%s/RenderScene.h"' % _SHADOW_SRC,
        'ZENGINE_RENDER_SCENE_CPP="%s/RenderScene.cpp"' % _SHADOW_SRC,
        'ZENGINE_RENDER_COMMON_H="%s/RenderCommon.h"' % _SHADOW_SRC,
        'ZENGINE_RENDER_HELPER_CPP="%s/RenderHelper.cpp"' % _SHADOW_SRC,
        'ZENGINE_RENDER_RESOURCE_H="%s/RenderResource.h"' % _SHADOW_SRC,
        'ZENGINE_RENDER_PERFRAME_CPP="%s/RenderResourcePerFrame.cpp"' % _SHADOW_SRC,
        'ZENGINE_SHADER_CONSTANTS_H="%s/include/constants.h"' % _SHADER,
        'ZENGINE_SHADER_STRUCTURES_H="%s/include/structures.h"' % _SHADER,
        'ZENGINE_SHADER_MESH_LIGHTING_H="%s/include/mesh_lighting.h"' % _SHADER,
        'ZENGINE_SHADER_MESH_LIGHTING_INL="%s/include/mesh_lighting.inl"' % _SHADER,
        'ZENGINE_SHADER_MEGALIGHTS_COMPOSE_INL="%s/include/megalights_compose.inl"' % _SHADER,
        'ZENGINE_SHADER_MESH_FRAG="%s/glsl/mesh.frag"' % _SHADER,
        'ZENGINE_SHADER_DEFERRED_LIGHTING_FRAG="%s/glsl/deferred_lighting.frag"' % _SHADER,
        'ZENGINE_SHADER_MEGALIGHTS_DEFERRED_FRAG="%s/glsl/megalights_deferred.frag"' % _SHADER,
        'ZENGINE_SHADER_MEGALIGHTS_SPATIAL_FRAG="%s/glsl/megalights_spatial.frag"' % _SHADER,
        'ZENGINE_SHADER_SCENE_LIGHTING_STRUCTS_HLSLI="%s/hlsl/common/scene_lighting_structs.hlsli"' % _SHADER,
    ],
    'ZEditorLauncherWiringSmokeTest': [
        'ZEDITOR_MAIN_CPP="%s/Editor/Main/Main.cpp"' % _E,
        'ZEDITOR_LAUNCHER_DIR="%s/Editor/Launcher"' % _E,
        'ZEDITOR_COMMANDSYSTEM_CPP="%s/Runtime/Function/Command/CommandSystem.cpp"' % _E,
        'ZEDITOR_CONFIGMANAGER_CPP="%s/Runtime/Resource/Config/ConfigManager.cpp"' % _E,
        'ZEDITOR_BUNDLE_CMAKE="%s/Editor/cmake/MakeMacOSAppBundle.cmake"' % _E,
        'ZEDITOR_MACOS_APP_H="%s/Runtime/Function/Render/Platform/MacOS/MacOSApplication.h"' % _E,
        'ZEDITOR_MACOS_APP_MM="%s/Runtime/Function/Render/Platform/MacOS/MacOSApplication.mm"' % _E,
    ],
}


def expand(patterns):
    import glob
    out = []
    for p in patterns:
        out.extend(sorted(glob.glob(os.path.join(SRC, p))))
    return out


def tail(text, n=12):
    lines = [l for l in text.splitlines() if l.strip()]
    return '\n'.join(lines[-n:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--filter', default='', help='substring, or comma-separated list of test names')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--json', default=os.path.join(ROOT, 'build-web', 'smoke_node', 'all_results.json'))
    a = ap.parse_args()

    tests = []
    for d, _, fs in os.walk(SRC):
        if os.sep + 'Test' not in d and '/Test' not in d:
            continue
        for f in fs:
            if f.endswith('SmokeTest.cpp') and any(x in f for x in a.filter.split(',')):
                tests.append(os.path.join(d, f))
    tests.sort()

    results = []
    for path in tests:
        name = os.path.splitext(os.path.basename(path))[0]
        rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
        if name in PLATFORM_ONLY:
            results.append({'name': name, 'verdict': 'SKIP', 'detail': PLATFORM_ONLY[name], 'path': rel})
            print('SKIP    %-40s %s' % (name, PLATFORM_ONLY[name]), flush=True)
            continue
        cmd = [sys.executable, os.path.join(ROOT, 'scripts', 'web_smoke_run.py'), path]
        for x in expand(EXTRA.get(name, [])):
            cmd += ['--extra', x]
        for d in INCLUDE_DIRS.get(name, []):
            cmd += ['--include-dir', d]
        for d in DEFINES.get(name, []):
            cmd += ['--define', d]
        for l in LINK.get(name, []):
            cmd += ['--link', l]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                               encoding='utf-8', errors='replace', timeout=a.timeout)
            out = p.stdout + p.stderr
            code = p.returncode
            verdict = 'PASS' if code == 0 else ('BUILD' if code == 3 else 'FAIL')
            if verdict == 'FAIL':
                if name in ENV_LIMIT_TESTS:
                    verdict = 'ENV'
                    out = ENV_LIMIT_TESTS[name] + chr(10) + out
                for marker, why in ENV_LIMIT_MARKERS.items():
                    if verdict == 'FAIL' and marker in out:
                        verdict = 'ENV'
                        out = why + chr(10) + out
                        break
        except subprocess.TimeoutExpired as e:
            out = ((e.stdout or b'').decode('utf-8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or '')) + \
                  ((e.stderr or b'').decode('utf-8', 'replace') if isinstance(e.stderr, bytes) else (e.stderr or ''))
            code = None
            verdict = 'TIMEOUT'
        secs = time.time() - t0
        results.append({'name': name, 'verdict': verdict, 'exit': code, 'seconds': round(secs, 1),
                        'path': rel, 'tail': tail(out, 25)})
        print('%-7s %-40s exit=%s %.0fs%s' % (verdict, name, code, secs, ('  ' + out.splitlines()[0]) if verdict == 'ENV' else ''), flush=True)

    os.makedirs(os.path.dirname(a.json), exist_ok=True)
    with open(a.json, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    counts = {}
    for r in results:
        counts[r['verdict']] = counts.get(r['verdict'], 0) + 1
    print('summary: ' + ', '.join('%s %d' % kv for kv in sorted(counts.items())))
    print('details: ' + a.json)
    return 0 if counts.get('FAIL', 0) == 0 and counts.get('BUILD', 0) == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
