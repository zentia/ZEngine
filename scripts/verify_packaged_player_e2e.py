#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Packaged-player end-to-end regression.

This is the third step of the "one real project must actually run" loop:

    build .zbundle  ->  launch a standalone ZGame from a project that has NO
    source asset tree  ->  prove the level came out of the bundles  ->  render
    frames  ->  quit unattended with a real exit code.

Why the isolated project matters
--------------------------------
Mounting bundles does not prove anything by itself: the engine falls back to
reading the source tree, so a broken package still boots "fine" on a dev
machine. This script copies a project that deliberately contains NO
``asset/<levels, prefabs, textures>`` and NO ``Intermediate/`` at all. The
startup level therefore CANNOT come from disk -- if ``level load succeed``
appears in the log, the bytes were served by ``SerializedFile::InitializeRead``
from a mounted ``.zbundle``. That is the assertion this whole test exists for.

It also catches the three failures found on 2026-11-05, all of which were
invisible to unit tests and to the editor:

  * AssetBundleBuilder resolved registry keys against GetProjectContent()
    instead of the registry scan root, producing "<project>/asset/asset/..."
    and skipping almost every asset.
  * An empty ``shared`` bundle was still declared as a dependency, so
    MountBuild reported "needs missing bundle 'shared'".
  * ``shared`` and ``scripts`` declared each other as dependencies, a cycle
    that made MountBuild mount exactly ZERO bundles while the player still
    booted from the source tree.

Usage (from the repo root, Windows / DX12)::

    python scripts/verify_packaged_player_e2e.py
    python scripts/verify_packaged_player_e2e.py --skip-pack --frames 60
    python scripts/verify_packaged_player_e2e.py --config release --frames 120

Exit codes: 0 passed, 1 an assertion failed, 2 prerequisites missing,
3 the pack or the player run timed out / was killed.
"""

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Directories a packaged player still needs from the project (shaders, script
# registry, icons, global textures). Deliberately NOT copied: `asset/` content
# subdirectories and the whole `Intermediate/` tree (cooked textures and
# compiled .js), because those are exactly what the bundles must replace.
PROJECT_DIR_WHITELIST = (
    "AssetRegistry",
    "Scripts",
    "Shaders",
    "shader",
    "resource",
    "configs",
    "Textures",
    "Models",
    "Data",
)
GLOBAL_ASSET_SUBDIR = "global"


class Failure(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code


def log(msg):
    print(f"[pak-e2e] {msg}", flush=True)


def fail(msg, code=1):
    raise Failure(msg, code)


def run_process(argv, timeout, label, stdout_path):
    log(f"{label}: {' '.join(str(a) for a in argv)}")
    started = time.time()
    # NEVER use stdout=PIPE for these GUI processes on Windows. ZEditor/ZGame
    # spawn helpers (RenderDoc, the Bq log manager, DX12 tooling) that inherit
    # the pipe; the main process exits but the pipe never reaches EOF, so
    # communicate() hangs forever. A real file has no EOF to wait for.
    with stdout_path.open("w", encoding="utf-8", errors="replace") as sink:
        try:
            proc = subprocess.run(
                [str(a) for a in argv],
                stdout=sink,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            fail(f"{label} timed out after {timeout}s (process killed, log may be incomplete)", 3)
    elapsed = time.time() - started
    log(f"{label}: exit={proc.returncode} elapsed={elapsed:.1f}s")
    return proc.returncode, stdout_path.read_text(encoding="utf-8", errors="replace")


def parse_zproject(path):
    """Minimal key: value reader; the .zproject format has no nesting we need."""
    values = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, _, value = line.partition(":")
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def newest_log(directory, prefix):
    candidates = sorted(
        directory.glob(f"{prefix}_*.log"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def read_added_text(path, offset):
    """Read only what this run appended; fall back to the whole file on rewrite."""
    try:
        size = path.stat().st_size
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            if size >= offset:
                handle.seek(offset)
                return handle.read()
            return handle.read()
    except OSError:
        return ""


def build_isolated_project(source_root, project_file, zproject, build_dir, out_root):
    """Copy a project with NO source assets and NO Intermediate/ tree."""
    # Never bulk-delete a previous isolated project: on a shared checkout that
    # is both slow and (rightly) blocked by workspace safety policies. A fresh
    # timestamped directory is cheaper and leaves the previous run's log intact
    # for diffing when something regresses.
    out_root.mkdir(parents=True, exist_ok=True)

    shutil.copy2(project_file, out_root / project_file.name)
    for name in PROJECT_DIR_WHITELIST:
        src = source_root / name
        if src.is_dir():
            shutil.copytree(src, out_root / name, dirs_exist_ok=True)

    # Only engine-global assets survive: no levels, no prefabs, no textures.
    global_src = source_root / "asset" / GLOBAL_ASSET_SUBDIR
    if global_src.is_dir():
        shutil.copytree(
            global_src, out_root / "asset" / GLOBAL_ASSET_SUBDIR, dirs_exist_ok=True
        )
    else:
        (out_root / "asset").mkdir(exist_ok=True)

    shutil.copytree(build_dir, out_root / "Build" / build_dir.name, dirs_exist_ok=True)
    return out_root / project_file.name


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", default=str(repo / "engine" / "ZGameSmoke.zproject"))
    parser.add_argument("--config", default="debug", choices=("debug", "release"))
    parser.add_argument("--editor", default=None, help="override ZEditor path")
    parser.add_argument("--player", default=None, help="override ZGame path")
    parser.add_argument("--platform", default="standalone")
    parser.add_argument("--rhi", default="dx12", choices=("dx12", "vulkan"))
    parser.add_argument("--frames", type=int, default=90, help="frames before quit")
    parser.add_argument("--out", default=str(repo / "build" / "_packaged_e2e"))
    parser.add_argument("--skip-pack", action="store_true", help="reuse existing bundles")
    parser.add_argument("--pack-timeout", type=int, default=300)
    parser.add_argument("--run-timeout", type=int, default=240)
    args = parser.parse_args()

    project_file = Path(args.project).resolve()
    source_root = project_file.parent
    zproject = parse_zproject(project_file)
    project_name = zproject.get("name") or project_file.stem
    startup_level = zproject.get("startup_level", "")
    platform_dir = {"standalone": "Standalone", "android": "Android", "ios": "iOS",
                    "ohos": "OHOS", "webgl": "WebGL"}.get(args.platform, args.platform)
    build_dir = source_root / "Build" / platform_dir
    bin_dir = repo / "bin" / args.config.capitalize()
    editor = Path(args.editor) if args.editor else bin_dir / "ZEditor.exe"
    player = Path(args.player) if args.player else bin_dir / "ZGame.exe"
    workspace = Path(args.out).resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    out_root = workspace / f"PlayerProject_{time.strftime('%Y%m%d_%H%M%S')}"

    for binary, label in ((editor, "ZEditor"), (player, "ZGame")):
        if not binary.is_file():
            fail(f"{label} not found: {binary} (build it first)", 2)
    if not project_file.is_file():
        fail(f"project not found: {project_file}", 2)
    if not startup_level:
        fail(".zproject has no startup_level -- nothing to prove", 2)

    # ---------------------------------------------------------------- pack --
    if not args.skip_pack:
        editor_log_dir = source_root / "Saved" / "Logs"
        editor_log_dir.mkdir(parents=True, exist_ok=True)
        before = newest_log(editor_log_dir, "ZEditor")
        offset = before.stat().st_size if before else 0

        code, stdout = run_process(
            [editor, "-p", project_file,
             "--cvar", f"bundle.build {args.platform} --force",
             "--cvar", "exec.after 10 quit"],
            args.pack_timeout, "pack", workspace / "pack_stdout.log",
        )
        if code != 0:
            fail(f"pack exited {code}; stdout tail:\n" + "\n".join(stdout.splitlines()[-20:]))

        after = newest_log(editor_log_dir, "ZEditor")
        pack_log = read_added_text(after, offset) if after else ""
        if "bundle.build:" not in pack_log:
            fail("no 'bundle.build:' line in the editor log -- the command never ran")
        if "cannot read asset" in pack_log:
            fail("pack skipped assets ('cannot read asset') -- check the registry-key base path")
        if re.search(r"/asset/asset/", pack_log):
            fail("pack resolved '<project>/asset/asset/...' -- registry key base is wrong")
        log("pack log: " + next((l.strip() for l in reversed(pack_log.splitlines())
                                 if "bundle.build:" in l), ""))

    manifest = build_dir / f"{project_name}.manifest.zasset"
    bundles = sorted((build_dir / "Bundles").glob("*.zbundle"))
    if not manifest.is_file():
        fail(f"manifest missing: {manifest}", 2)
    if not bundles:
        fail(f"no .zbundle under {build_dir / 'Bundles'}", 2)
    log(f"pack artifacts: {len(bundles)} bundle(s), manifest {manifest.name}")

    # ------------------------------------------------------------ isolated --
    isolated_project = build_isolated_project(
        source_root, project_file, zproject, build_dir, out_root
    )
    # The whole point: the startup level must NOT exist on disk.
    level_on_disk = out_root / startup_level
    if level_on_disk.exists():
        fail(f"isolated project still contains the startup level at {level_on_disk}")
    if (out_root / "Intermediate").exists():
        fail("isolated project still contains Intermediate/")
    log(f"isolated project: {out_root} (startup level '{startup_level}' absent on disk)")

    # --------------------------------------------------------------- player --
    code, stdout = run_process(
        [player, "-p", isolated_project, "--rhi", args.rhi,
         "--cvar", f"exec.after {args.frames} quit"],
        args.run_timeout, "player", workspace / "player_stdout.log",
    )
    if code != 0:
        fail(f"player exited {code}; stdout tail:\n" + "\n".join(stdout.splitlines()[-20:]))

    player_log_dir = out_root / "Saved" / "Logs"
    log_file = newest_log(player_log_dir, "ZGame")
    if log_file is None:
        fail(f"no ZGame log under {player_log_dir}")
    text = log_file.read_text(encoding="utf-8", errors="replace")

    checks = []

    def check(condition, name, detail=""):
        checks.append((name, bool(condition), detail))
        if not condition:
            log(f"[FAIL] {name}" + (f" -- {detail}" if detail else ""))

    mounted = re.search(r"AssetBundleManager: (\d+) bundle\(s\) mounted", text)
    check(mounted is not None and int(mounted.group(1)) > 0,
          "bundles mounted", "no 'N bundle(s) mounted' line")
    check(not re.search(r"\[E\].*\[ZPak\]", text),
          "no ZPak errors", "mount failed -- see the log")
    check("level load succeed" in text, "level loaded", "see the log")
    # The decisive assertion. "0 bundle(s) mounted + level load succeed" was
    # once green because ReadObjectsFromYaml did a plain fopen against the
    # engine source tree. Require an explicit bundle hit for the startup level.
    level_name = Path(startup_level).name
    check(re.search(r"ReadObjectsFromYaml: '[^']*" + re.escape(level_name) +
                    r"' served from a mounted bundle", text) is not None,
          "level bytes served from a bundle",
          "the YAML reader fell back to the source tree -- packaged player is NOT self-contained")
    graph = re.search(r"level object graph built \((\d+) objects\)", text)
    check(graph is not None and int(graph.group(1)) > 0,
          "non-empty object graph", "an empty level also reports 'level load succeed'")
    check("OpenScene: loaded" in text, "scene opened")
    check(f"exec.after: 'quit' in {args.frames} frame" in text,
          "frame-deferred command scheduled", "--cvar plumbing is broken")
    check("Quit requested" in text and "All systems shut down" in text,
          "clean unattended shutdown")
    check(any(marker in text for marker in
              ("render entities exist", "RenderGraph A/B capture: recording frame",
               "MainCameraRp1Pass: RP1 is driven")),
          "render path ran at least one frame")

    pak_errors = len(re.findall(r"\[E\].*\[ZPak\]", text))
    render_errors = len(re.findall(r"\[E\].*\[ZRender\]", text))
    log(f"log: {log_file}")
    log(f"mounted={mounted.group(1) if mounted else '?'} "
        f"ZPak_errors={pak_errors} ZRender_errors={render_errors} "
        f"(DX12 debug-layer errors on shutdown are a known, tracked issue)")

    passed = sum(1 for _, ok, _ in checks if ok)
    for name, ok, _ in checks:
        log(f"  {'PASS' if ok else 'FAIL'}  {name}")
    log(f"{passed}/{len(checks)} checks passed")
    if passed != len(checks):
        fail("packaged-player e2e failed; see the FAIL lines above", 1)
    log("PASS - packaged content booted a standalone player without a source asset tree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as exc:
        log(f"FAILED: {exc}")
        sys.exit(exc.code)
