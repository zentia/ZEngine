#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stage a REAL, source-free Player installation and prove a packaged behaviour ticks.

This is the strict form of the packaged-player verification (PR-PAK1 follow-up,
defects #7 and #9). The install directory contains ONLY:

    ZGame.exe + its DLLs, ZEditor.ini, the .zproject, Build/Standalone/*

There is deliberately NO asset/ tree, NO .scene, NO *.ts, NO *.js and NO loose
AssetRegistry/script_registry.json. Everything the runtime reads -- level YAML,
engine-global configs, component JSON, the script registry and the compiled
TypeScript module -- has to come out of a mounted .zbundle. A
TypeScriptComponent that still ticks OnUpdate for N frames with no script source
or compiled mirror anywhere on disk is the proof that scripts.zbundle was used.

Earlier revisions of this script used to stage ``asset/global/`` (engine config
copied out of the source tree) plus a .ts metadata copy and a deliberately
broken .js decoy. Those were scaffolding for two real defects:

  #7  FileSystem::loadAssetJson() was a plain fopen(), and AssetBundleBuilder
      never shipped engine-global configs or component-referenced
      *.material.json / *.skeleton.json. A dev machine silently read them from
      the source tree; an installed player logged "open file ... failed".
  #9  ScriptRegistry::rescan() clears the whole table when Scripts/ is absent
      and wipes m_CompiledRelPath when Intermediate/Scripts/*.js is absent.
      A packaged player should carry neither.

Both are fixed: all JSON/YAML readers now ask AssetBundleManager first, the
builder ships the text assets and the registry JSON in the shared bundle, and
the player shell mounts the raw bundles BEFORE START_SYSTEM() (ParticleManager,
RenderSystem and ScriptRegistry read JSON during Initialize()) and marks the
script registry as shipped metadata.

Chain (all from a clean generated project, no repository source assets touched):

    author .zproject + .scene + .ts + .js + script_registry.json
      -> ZEditor `bundle.build standalone --force`
      -> cmake --install --component ZGamePlayer (the REAL installer, not a
         Python copy helper) puts ONLY exe + DLLs + ini + .zproject +
         Build/Standalone into a fresh install dir
      -> launch the installed Player there
      -> assert level/global/registry bytes served from bundles,
         TypeScriptComponent bound, OnUpdate ran for N frames, clean shutdown.

Usage (from the repo root, Windows / DX12)::

    python scripts/verify_player_install_e2e.py
    python scripts/verify_player_install_e2e.py --frames 60
    python scripts/verify_player_install_e2e.py --out C:/Temp/zengine-install-e2e
    python scripts/verify_player_install_e2e.py --stage-mode copy  # control

Exit codes: 0 passed, 1 assertion failed, 2 prerequisites missing,
3 pack or player timed out / was killed.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

PROBE_GUID = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
PROBE_MODULE = "ZPakProbe"
PROBE_SCENE = "asset/level/probe.scene"

ZPROJECT = """name: ZProbe
description: Minimal packaged-player probe carrying a TypeScriptComponent.
version: 1.0.0
engine_version: 0.1.0
startup_level: asset/level/probe.scene
content_dir: asset
config_dir: config
saved_dir: saved
source_dir: source
scripts_dir: Scripts
intermediate_dir: Intermediate
shaders_dir: Shaders
data_dir: Data
textures_dir: Textures
models_dir: Models
"""

# A plain authored scene: one GameObject with a Transform and a bound
# TypeScriptComponent. Deliberately no meshes/materials so a missing content
# dependency cannot be mistaken for a scripting failure.
SCENE = """externals: []
objects:
  - fileID: 1
    type: LevelRes
    data:
      LevelRes:
        gravity:
          x: 0
          y: 0
          z: -9.8000001907348633
        character_name: Player
  - fileID: 2
    type: GameObject
    data:
      GameObject:
        m_Name: ZPakProbe
        m_DefinitionUrl: ""
        m_Component:
          - component:
              m_FileID: 0
              m_PathID: 3
          - component:
              m_FileID: 0
              m_PathID: 4
  - fileID: 3
    type: Transform
    data:
      Transform:
        enabled: true
        m_LocalRotation:
          w: 1
          x: 0
          y: 0
          z: 0
        m_LocalPositionD:
          x: 0
          y: 0
          z: 0
        m_LocalPosition:
          x: 0
          y: 0
          z: 0
        m_LocalScale:
          x: 1
          y: 1
          z: 1
        m_Father:
          m_FileID: 0
          m_PathID: 0
        m_Children: []
  - fileID: 4
    type: TypeScriptComponent
    data:
      TypeScriptComponent:
        enabled: true
        m_script_guid: __GUID__
        m_class_name: ""
        m_serialized_fields: []
"""

TS_SOURCE = """export class ZPakProbe extends Behaviour {
    private frames = 0;
    OnStart(): void { console.log("[ZPakProbe] OnStart"); }
    OnUpdate(dt: number): void {
        ++this.frames;
        console.log("[ZPakProbe] OnUpdate frame " + this.frames + " dt=" + dt.toFixed(4));
    }
}
"""

# tsc emits CommonJS-flavoured JS. The editor also runs tsc on startup, so this
# file is only a seed; a syntactically valid .ts above keeps that recompile
# equivalent. It lives in the AUTHORING project only -- never in the install.
JS_COMPILED = '''"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.ZPakProbe = void 0;
class ZPakProbe extends Behaviour {
    constructor() {
        super(...arguments);
        this.frames = 0;
    }
    OnStart() { console.log("[ZPakProbe] OnStart"); }
    OnUpdate(dt) {
        ++this.frames;
        console.log("[ZPakProbe] OnUpdate frame " + this.frames + " dt=" + dt.toFixed(4));
    }
}
exports.ZPakProbe = ZPakProbe;
'''


class Failure(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code


def log(msg):
    print(f"[player-install-e2e] {msg}", flush=True)


def fail(msg, code=1):
    raise Failure(msg, code)


def run_process(argv, timeout, label, stdout_path, cwd=None, env=None):
    log(f"{label}: {' '.join(str(a) for a in argv)}")
    started = time.time()
    # Never use stdout=PIPE for ZEditor/ZGame on Windows: spawned helpers
    # inherit the pipe and the parent exits before EOF, hanging communicate().
    with stdout_path.open("w", encoding="utf-8", errors="replace") as sink:
        try:
            proc = subprocess.run(
                [str(a) for a in argv],
                stdout=sink,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                cwd=str(cwd) if cwd else None,
                env=env,
            )
        except subprocess.TimeoutExpired:
            fail(f"{label} timed out after {timeout}s (process killed)", 3)
    log(f"{label}: exit={proc.returncode} elapsed={time.time() - started:.1f}s")
    return proc.returncode, stdout_path.read_text(encoding="utf-8", errors="replace")


def write_probe_project(out_root):
    """Author a complete minimal project. Never touches repository assets."""
    out_root.mkdir(parents=True, exist_ok=True)
    (out_root / "ZProbe.zproject").write_text(ZPROJECT, encoding="utf-8")
    (out_root / "asset" / "level").mkdir(parents=True, exist_ok=True)
    (out_root / "asset" / "level" / "probe.scene").write_text(
        SCENE.replace("__GUID__", PROBE_GUID), encoding="utf-8")
    (out_root / "Scripts").mkdir(exist_ok=True)
    (out_root / "Scripts" / f"{PROBE_MODULE}.ts").write_text(TS_SOURCE, encoding="utf-8")
    (out_root / "Intermediate" / "Scripts").mkdir(parents=True, exist_ok=True)
    (out_root / "Intermediate" / "Scripts" / f"{PROBE_MODULE}.js").write_text(
        JS_COMPILED, encoding="utf-8")
    (out_root / "AssetRegistry").mkdir(exist_ok=True)
    registry = {
        "version": 1,
        "entries": [{
            "guid": PROBE_GUID,
            "path": f"Scripts/{PROBE_MODULE}.ts",
            "class": PROBE_MODULE,
            "mtime": 0,
            "content_hash": "0",
            "compiled": f"Intermediate/Scripts/{PROBE_MODULE}.js",
        }],
    }
    (out_root / "AssetRegistry" / "script_registry.json").write_text(
        json.dumps(registry, indent=4), encoding="utf-8")
    return out_root / "ZProbe.zproject"


def verify_source_free(install_root):
    """The install layout must contain NO authored source asset of any kind."""
    # The engine creates some of these DIRECTORIES at runtime (e.g. an empty
    # asset/ when resolving a config URL). Empty directories are harmless; what
    # is forbidden is any authored file inside them.
    for name in ("asset", "AssetRegistry", "Scripts", "Intermediate"):
        tree = install_root / name
        if tree.is_dir() and any(p.is_file() for p in tree.rglob("*")):
            fail(f"install dir must NOT contain shipped files under {name}/: {tree}")

    stray = sorted(
        p.relative_to(install_root).as_posix()
        for p in install_root.rglob("*")
        if p.is_file()
        and p.suffix.lower() in (".ts", ".js", ".scene", ".json")
        and p.relative_to(install_root).parts[0] != "Build"
    )
    if stray:
        fail(f"install dir contains loose source/JSON assets: {', '.join(stray[:8])}")
    return True


def stage_install(probe_root, install_root, bin_dir, build_dir, config, mode,
                  workspace, stamp):
    """Produce the source-free install, by default via the REAL installer.

    ``cmake`` runs `cmake --install --component ZGamePlayer` with
    ZENGINE_PLAYER_PROJECT pointing at the freshly packaged project. An install
    rule that regresses (DLLs in bin/, shipped .scene files, missing ini)
    therefore turns this verification red instead of being masked by a Python
    shutil.copy2 helper. ``copy`` keeps the old hand-rolled staging as a
    control / fallback when no configured build tree is available.
    """
    exe = bin_dir / "ZGame.exe"
    ini = bin_dir / "ZEditor.ini"
    if not exe.is_file() or not ini.is_file():
        fail(f"missing {exe} or {ini} (build the engine first)", 2)
    build_src = probe_root / "Build" / "Standalone"
    if not build_src.is_dir():
        fail(f"packaged build missing: {build_src}", 2)

    if mode == "cmake":
        cmake_install = build_dir / "cmake_install.cmake"
        if not cmake_install.is_file():
            fail(f"build tree is not configured: {cmake_install}", 2)
        env = os.environ.copy()
        env["ZENGINE_PLAYER_PROJECT"] = str(probe_root)
        code, stdout = run_process(
            ["cmake", "--install", str(build_dir),
             "--config", config.capitalize(),
             "--component", "ZGamePlayer",
             "--prefix", str(install_root)],
            300, "cmake --install", workspace / f"cmake_install_{stamp}.log",
            env=env,
        )
        if code != 0:
            fail(f"cmake --install exited {code}; output tail:\n"
                 + "\n".join(stdout.splitlines()[-20:]), 2)
        dlls = sorted(install_root.glob("*.dll"))
        for required, label in (
            (install_root / "ZGame.exe", "ZGame.exe"),
            (install_root / "ZEditor.ini", "ZEditor.ini"),
            (install_root / "ZProbe.zproject", "ZProbe.zproject"),
            (install_root / "Build" / "Standalone" / "ZProbe.manifest.zasset",
             "packaged manifest"),
        ):
            if not required.is_file():
                fail(f"cmake --install did not produce {label}: {required}", 2)
    else:
        install_root.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe, install_root / exe.name)
        dlls = sorted(bin_dir.glob("*.dll"))
        if not dlls:
            fail(f"no DLLs next to {exe} -- the Player cannot start", 2)
        for dll in dlls:
            shutil.copy2(dll, install_root / dll.name)
        shutil.copy2(ini, install_root / ini.name)
        shutil.copy2(probe_root / "ZProbe.zproject", install_root / "ZProbe.zproject")
        shutil.copytree(build_src, install_root / "Build" / "Standalone",
                        dirs_exist_ok=True)
        stdout = ""
    if not dlls:
        fail("cmake --install produced no runtime DLLs next to ZGame.exe", 2)

    # Hard invariants of the fixture itself. With #7 / #9 fixed, config JSON,
    # the script registry and the compiled module all come from bundles.
    if (install_root / PROBE_SCENE).exists():
        fail(f"install dir must NOT contain the startup level: {install_root / PROBE_SCENE}")
    verify_source_free(install_root)
    log(f"install dir: {install_root} ({len(dlls)} DLLs, zero source assets, "
        f"mode={mode})")
    return install_root, stdout


def newest_log(directory, prefix):
    candidates = sorted(directory.glob(f"{prefix}_*.log"),
                        key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0] if candidates else None


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="debug", choices=("debug", "release"))
    parser.add_argument("--editor", default=None, help="override ZEditor path")
    parser.add_argument("--player", default=None, help="override ZGame path")
    parser.add_argument("--rhi", default="dx12", choices=("dx12", "vulkan"))
    parser.add_argument("--frames", type=int, default=30, help="frames before quit")
    parser.add_argument("--out", default=str(repo / "build" / "_player_install_e2e"),
                        help="workspace for generated project + install dir; "
                             "may point outside the repository for full isolation")
    parser.add_argument("--pack-timeout", type=int, default=300)
    parser.add_argument("--run-timeout", type=int, default=240)
    parser.add_argument("--stage-mode", default="cmake", choices=("cmake", "copy"),
                        help="cmake: use the real ZGamePlayer install component "
                             "(default); copy: hand-rolled staging as a control")
    parser.add_argument("--build-dir", default=None,
                        help="configured CMake build tree (default: <repo>/build)")
    args = parser.parse_args()

    bin_dir = repo / "bin" / args.config.capitalize()
    editor = Path(args.editor) if args.editor else bin_dir / "ZEditor.exe"
    workspace = Path(args.out).resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    probe_root = workspace / f"ProbeProject_{stamp}"
    install_root = workspace / f"PlayerInstall_{stamp}"

    for binary, label in ((editor, "ZEditor"), (bin_dir / "ZGame.exe", "ZGame")):
        if not binary.is_file():
            fail(f"{label} not found: {binary} (build it first)", 2)

    # ---------------------------------------------------------------- author --
    project_file = write_probe_project(probe_root)
    log(f"authored probe project: {probe_root}")

    # ------------------------------------------------------------------ pack --
    code, stdout = run_process(
        [editor, "-p", project_file,
         "--cvar", "bundle.build standalone --force",
         "--cvar", "exec.after 10 quit"],
        args.pack_timeout, "pack", workspace / f"pack_stdout_{stamp}.log",
    )
    if code != 0:
        fail(f"pack exited {code}; stdout tail:\n" + "\n".join(stdout.splitlines()[-20:]))
    pack_log_file = newest_log(probe_root / "Saved" / "Logs", "ZEditor")
    if pack_log_file is None or "bundle.build:" not in pack_log_file.read_text(
            encoding="utf-8", errors="replace"):
        fail("no 'bundle.build:' line in the editor log -- the command never ran")

    build_dir = probe_root / "Build" / "Standalone"
    manifest = build_dir / "ZProbe.manifest.zasset"
    bundles = sorted((build_dir / "Bundles").glob("*.zbundle"))
    if not manifest.is_file() or len(bundles) < 2:
        fail(f"pack produced {len(bundles)} bundle(s); expected a level bundle "
             f"and scripts.zbundle plus {manifest.name}", 2)
    log(f"pack artifacts: {len(bundles)} bundle(s)")

    # --------------------------------------------------------------- install --
    build_dir = Path(args.build_dir) if args.build_dir else repo / "build"
    install_root, install_stdout = stage_install(
        probe_root, install_root, bin_dir, build_dir, args.config,
        args.stage_mode, workspace, stamp)
    # Snapshot BEFORE the player runs: the engine creates empty asset/ and
    # Saved/ directories at startup, which must not be mistaken for shipped
    # source assets later.
    source_free = verify_source_free(install_root)

    # ---------------------------------------------------------------- player --
    code, stdout = run_process(
        [install_root / "ZGame.exe", "-p", install_root / "ZProbe.zproject",
         "--rhi", args.rhi,
         "--cvar", f"exec.after {args.frames} quit"],
        args.run_timeout, "installed player", workspace / f"player_stdout_{stamp}.log",
        cwd=install_root,
    )
    if code != 0:
        fail(f"installed player exited {code}; stdout tail:\n"
             + "\n".join(stdout.splitlines()[-20:]))

    log_file = newest_log(install_root / "Saved" / "Logs", "ZGame")
    if log_file is None:
        fail(f"no ZGame log under {install_root / 'Saved' / 'Logs'}")
    text = log_file.read_text(encoding="utf-8", errors="replace")
    log(f"player log: {log_file}")

    checks = []

    def check(condition, name, detail=""):
        checks.append((name, bool(condition), detail))
        if not condition:
            log(f"[FAIL] {name}" + (f" -- {detail}" if detail else ""))

    if args.stage_mode == "cmake":
        check("-- Installing:" in install_stdout and
              re.search(r"ZGamePlayer: \d+ runtime DLL\(s\) installed",
                        install_stdout) is not None,
              "cmake --install produced the Player layout",
              "the real installer did not stage exe + runtime DLLs")
        check("ZGamePlayer: staged packaged project" in install_stdout,
              "cmake --install staged the packaged project",
              "ZENGINE_PLAYER_PROJECT was ignored by the install component")
    else:
        log("stage-mode=copy: skipping the two installer assertions (control run)")

    mounted = re.search(r"AssetBundleManager: (\d+) bundle\(s\) mounted", text)
    check(mounted is not None and int(mounted.group(1)) >= 2,
          "level + script bundles mounted", "mount failed -- see the log")
    check(not re.search(r"\[E\].*\[ZPak\]", text),
          "no ZPak errors", "a bundle failed to mount or open")
    early = re.search(r"early-mounted (\d+) bundle\(s\) before system startup", text)
    check(early is not None and int(early.group(1)) >= 2,
          "bundles mounted before START_SYSTEM (#7 timing)",
          "early readers would have fallen back to disk")
    check(re.search(r"ReadObjectsFromYaml: '[^']*probe\.scene' served from a mounted bundle",
                    text) is not None,
          "level bytes served from a bundle",
          "the YAML reader fell back to disk")
    graph = re.search(r"level object graph built \((\d+) objects\)", text)
    check(graph is not None and int(graph.group(1)) > 0,
          "non-empty object graph", "an empty level also reports 'level load succeed'")
    check("level load succeed" in text and "OpenScene: loaded" in text,
          "scene opened")
    check(re.search(r"FileSystem: '[^']*particle\.global\.json' served from a mounted bundle",
                    text) is not None,
          "engine-global config served from a bundle (#7)",
          "ParticleManager read the config off disk (or failed to find it)")
    check(re.search(r"FileSystem: '[^']*script_registry\.json' served from a mounted bundle",
                    text) is not None,
          "script registry served from a bundle (#9)",
          "ScriptRegistry fell back to a loose JSON file")
    check(re.search(r"open file[^\n]*\.json[^\n]*failed", text) is None,
          "no missing authored JSON",
          "a component/global JSON was still looked up on disk")
    check(re.search(rf"LoadModule\('{PROBE_MODULE}'\): \d+ bytes served from a mounted bundle",
                    text) is not None,
          "compiled module bytes served from scripts.zbundle (#9)",
          "ScriptEnv fell back to a .js file on disk")
    check(f"TypeScriptComponent bound: module='{PROBE_MODULE}'" in text,
          "TypeScriptComponent bound",
          "script registry / module id / compiled path did not resolve")
    check("[ZPakProbe] OnStart" in text, "script OnStart ran")
    updates = re.findall(r"\[ZPakProbe\] OnUpdate frame (\d+)", text)
    check(len(updates) >= 2, "script OnUpdate ticked for multiple frames",
          "the Player never drove component Tick (g_isPlaying / editor whitelist)")
    check("OnUpdate frame 1 " in text and
          f"OnUpdate frame {args.frames} " in text,
          "OnUpdate reached the requested frame count",
          f"frames seen: {','.join(updates[:8])}")
    check(source_free,
          "zero source assets on disk yet the module executed",
          "install layout regressed to shipping .ts/.js/global JSON")
    check("MainCameraRp1Pass: RP1 is driven" in text or
          "recording frame" in text,
          "render path ran at least one frame")
    check(f"exec.after: 'quit' in {args.frames} frame" in text,
          "frame-deferred command scheduled")
    check("Quit requested" in text and "All systems shut down" in text,
          "clean unattended shutdown")

    passed = sum(1 for _, ok, _ in checks if ok)
    for name, ok, _ in checks:
        log(f"  {'PASS' if ok else 'FAIL'}  {name}")
    log(f"{passed}/{len(checks)} checks passed; OnUpdate frames logged: "
        f"{','.join(updates[:10])}")
    if passed != len(checks):
        fail("installed-player e2e failed; see the FAIL lines above", 1)
    log("PASS - source-free installed Player booted entirely from bundles "
        "and its TypeScript behaviour ticked")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as exc:
        log(f"FAILED: {exc}")
        sys.exit(exc.code)
