#!/usr/bin/env python3
"""
ZEngine iOS 出包脚本（P0-1 移动端出包链）

做四件事：
  1. 把项目内容（含 Build/iOS/Bundles）staging 到
     engine/Source/Player/IOS/staging/zengine/ —— CMake 会把整棵声明成
     .app 的 bundle resource。
  2. cmake -G Xcode 生成工程（iOS toolchain + 真机 arm64）。
  3. xcodebuild 编出 .app；--export 时再走 archive + export 出 .ipa。
  4. 报告产物路径。

用法：
    python3 zbuild_ios.py --project /path/to/MyProject --config Release
    python3 zbuild_ios.py --project /path/to/MyProject --team XXXXXXXXXX --export

前置条件：
    macOS + Xcode（命令行工具不够，要完整的 Xcode.app，因为要签名与 Archive）。
    --export 需要有效的签名身份：-D 团队 ID 或 Xcode 里选好 Automatic signing。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
ENGINE_DIR = REPO_ROOT / "engine"
IOS_PROJECT_DIR = ENGINE_DIR / "Source" / "Player" / "IOS"
STAGING_DIR = IOS_PROJECT_DIR / "staging"
STAGING_PROJECT = STAGING_DIR / "zengine"
TOOLCHAIN_FILE = ENGINE_DIR / "toolchain" / "ios.cmake"
BUILD_DIR = REPO_ROOT / "build" / "ios"

EXCLUDED_DIR_NAMES = {".git", ".svn", ".vs", ".idea", "Intermediate", "Saved", "Logs"}
KEEP_BUILD_SUBDIRS = {"iOS"}


def log(msg: str) -> None:
    print(f"[zbuild-ios] {msg}")


def fail(msg: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"[zbuild-ios] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def require_xcode() -> None:
    if shutil.which("xcodebuild") is None:
        fail("找不到 xcodebuild：iOS 出包需要完整 Xcode（命令行工具不够）。")
    result = subprocess.run(["xcodebuild", "-version"], capture_output=True, text=True)
    if result.returncode != 0:
        fail("xcodebuild 跑不起来，先执行 `sudo xcode-select -s /Applications/Xcode.app`。")
    log("Xcode: " + result.stdout.strip().splitlines()[0])


def stage_project(project_root: Path) -> int:
    if STAGING_PROJECT.exists():
        log(f"清理旧 staging: {STAGING_PROJECT}")
        shutil.rmtree(STAGING_PROJECT)
    STAGING_PROJECT.mkdir(parents=True, exist_ok=True)

    copied = 0
    for src in project_root.rglob("*"):
        if src.is_dir():
            continue
        rel = src.relative_to(project_root)
        if any(part in EXCLUDED_DIR_NAMES for part in rel.parts):
            continue
        # 只带 iOS 的构建产物（Bundles / manifest 都在 Build/<Platform> 下）。
        if rel.parts[0] == "Build" and (len(rel.parts) < 2 or rel.parts[1] not in KEEP_BUILD_SUBDIRS):
            continue

        dst = STAGING_PROJECT / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        copied += 1

    log(f"staging: {copied} 个文件 -> {STAGING_PROJECT}")
    if copied == 0:
        fail(f"项目目录里没有任何内容可打进包：{project_root}")
        raise SystemExit(1)
    return copied


def run(cmd: list[str], cwd: Path | None = None) -> None:
    log("运行: " + " ".join(cmd))
    result = subprocess.run(cmd, cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        fail(f"命令失败（退出码 {result.returncode}）: {' '.join(cmd)}")


def configure(config: str, team: str | None, generator_args: str) -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [
        "cmake",
        "-S", str(ENGINE_DIR),
        "-B", str(BUILD_DIR),
        "-G", "Xcode",
        f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN_FILE}",
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DZENGINE_IOS_STAGING_DIR={STAGING_DIR}",
    ]
    if team:
        cmd.append(f"-DZENGINE_IOS_DEVELOPMENT_TEAM={team}")
    if generator_args:
        cmd += generator_args.split()
    run(cmd)


def build_app(config: str) -> Path:
    run(["xcodebuild",
         "-project", str(BUILD_DIR / "ZEngine.xcodeproj"),
         "-scheme", "ZGameIOS",
         "-configuration", config,
         "-destination", "generic/platform=iOS",
         "build"], cwd=BUILD_DIR)

    # 不要硬猜一个路径。Xcode 生成器的产物位置随 CMake 版本和
    # CMAKE_XCODE_OUTPUT_DIRECTORY 变（`<Config>-iphoneos/` 与 `bin/<Config>/`
    # 都见过），猜错就是在 xcodebuild 成功之后报"没找到 .app"。
    #
    # 先把几个已知位置列出来，都没有再 rglob —— rglob 能兜住，但它在整棵
    # build 树里找，慢，且可能捞到上一次的陈产物。
    candidates = [
        BUILD_DIR / f"{config}-iphoneos" / "ZEngine.app",
        BUILD_DIR / "bin" / config / "ZEngine.app",
    ]
    for candidate in candidates:
        if candidate.is_dir():
            return candidate

    fallback = sorted(BUILD_DIR.rglob("ZEngine.app"))
    if not fallback:
        fail("编完了但没找到 ZEngine.app"
             f"（找过 {[str(c) for c in candidates]}，也在整个 build 目录里搜过）")
        raise SystemExit(1)

    log(f"产物不在预期位置，rglob 找到: {fallback[0]}")
    return fallback[0]


def archive_and_export(config: str, team: str | None) -> Path:
    archive = BUILD_DIR / "ZEngine.xcarchive"
    if archive.exists():
        shutil.rmtree(archive)

    cmd = ["xcodebuild",
           "-project", str(BUILD_DIR / "ZEngine.xcodeproj"),
           "-scheme", "ZGameIOS",
           "-configuration", config,
           "-destination", "generic/platform=iOS",
           "-archivePath", str(archive),
           "archive"]
    if team:
        cmd.append(f"DEVELOPMENT_TEAM={team}")
    run(cmd, cwd=BUILD_DIR)

    export_options = BUILD_DIR / "ExportOptions.plist"
    export_options.write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n<dict>\n'
        '    <key>method</key>\n    <string>development</string>\n'
        '    <key>compileBitcode</key>\n    <false/>\n'
        '    <key>signingStyle</key>\n    <string>automatic</string>\n'
        f'    <key>teamID</key>\n    <string>{team or ""}</string>\n'
        '</dict>\n</plist>\n',
        encoding="utf-8")

    export_dir = BUILD_DIR / "export"
    if export_dir.exists():
        shutil.rmtree(export_dir)
    export_dir.mkdir(parents=True)

    run(["xcodebuild",
         "-exportArchive",
         "-archivePath", str(archive),
         "-exportPath", str(export_dir),
         "-exportOptionsPlist", str(export_options)], cwd=BUILD_DIR)

    ipas = sorted(export_dir.glob("*.ipa"))
    if not ipas:
        fail(f"导出完了但没找到 .ipa（{export_dir}）")
        raise SystemExit(1)
    return ipas[0]


def main() -> None:
    parser = argparse.ArgumentParser(description="ZEngine iOS 出包")
    parser.add_argument("--project", required=True, type=Path,
                        help="项目目录（需含 Build/iOS/Bundles 与 .zproject）")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release", "RelWithDebInfo"])
    parser.add_argument("--team", default=os.environ.get("ZENGINE_IOS_DEVELOPMENT_TEAM"),
                        help="签名用的 Apple Developer Team ID")
    parser.add_argument("--export", action="store_true", help="Archive 并导出 .ipa")
    parser.add_argument("--skip-staging", action="store_true",
                        help="跳过 staging（复用上一次的项目内容，加快迭代）")
    parser.add_argument("--cmake-args", default="", help="透传给 cmake 的额外参数")
    args = parser.parse_args()

    project_root: Path = args.project.resolve()
    if not project_root.is_dir():
        fail(f"项目目录不存在: {project_root}")
    if not (project_root / "Build" / "iOS").is_dir():
        fail(f"没有找到 {project_root / 'Build' / 'iOS'}："
             "先在编辑器里对 iOS 平台做一次打包。")

    require_xcode()

    if not args.skip_staging:
        stage_project(project_root)

    configure(args.config, args.team, args.cmake_args)

    if args.export:
        artifact = archive_and_export(args.config, args.team)
    else:
        artifact = build_app(args.config)

    if artifact.is_dir():
        log(f"出包完成（.app）: {artifact}")
    else:
        size_mb = artifact.stat().st_size / (1024 * 1024)
        log(f"出包完成（.ipa）: {artifact} ({size_mb:.1f} MB)")


if __name__ == "__main__":
    main()
