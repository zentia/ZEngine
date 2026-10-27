#!/usr/bin/env python3
"""
ZEngine Android 出包脚本（P0-1 移动端出包链）

做三件事：
  1. 把项目内容（含 Build/Android/Bundles）填进 Gradle 工程的 assets/zengine/，
     随 APK 打进去 —— 引擎启动时由 AndroidMain.cpp 解到 app 内部目录。
  2. 调 Gradle 编 libZGameAndroid.so 并打出 APK / AAB。
  3. 可选：adb install + 启动。

用法：
    python3 zbuild_android.py --project /path/to/MyProject --config debug
    python3 zbuild_android.py --project /path/to/MyProject --config release --aab
    python3 zbuild_android.py --project /path/to/MyProject --install

前置条件：
    ANDROID_HOME（或 ANDROID_SDK_ROOT）指向 Android SDK，且装了 NDK。
    第一次用之前需要 Gradle wrapper：本仓库只提交了 wrapper 配置，没提交
    gradlew 与 gradle-wrapper.jar（二进制）。用系统 gradle 生成一次即可：
        cd engine/Source/Player/Android && gradle wrapper --gradle-version 8.7
    或者直接用系统 gradle 跑（本脚本会自动回退到 PATH 里的 gradle）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
ANDROID_PROJECT_DIR = REPO_ROOT / "engine" / "Source" / "Player" / "Android"
ASSETS_DEST = ANDROID_PROJECT_DIR / "src" / "main" / "assets" / "zengine"

# 不随包打进去的目录：构建中间产物、其他平台的产物、版本控制目录。
EXCLUDED_DIR_NAMES = {
    ".git", ".svn", ".vs", ".idea",
    "Intermediate", "Saved", "Logs", "obj",
}

# Build 下只带 Android 的产物（其他平台/其他配置的 bundle 不带）。
KEEP_BUILD_SUBDIRS = {"Android"}


def log(msg: str) -> None:
    print(f"[zbuild-android] {msg}")


def fail(msg: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"[zbuild-android] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def find_android_sdk() -> Path:
    for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(var)
        if value and Path(value).is_dir():
            return Path(value)
    guess = Path.home() / "Library" / "Android" / "sdk"
    if guess.is_dir():
        return guess
    fail("找不到 Android SDK：请设置 ANDROID_HOME（或 ANDROID_SDK_ROOT）。")
    raise SystemExit(1)


def find_gradle() -> list[str]:
    wrapper = ANDROID_PROJECT_DIR / "gradlew"
    if wrapper.is_file():
        return [str(wrapper)]
    which = shutil.which("gradle")
    if which:
        log("没有 gradlew，回退到 PATH 里的 gradle（建议跑一次 `gradle wrapper` 固定版本）")
        return [which]
    fail("找不到 gradle：请先在 engine/Source/Player/Android 下跑 "
         "`gradle wrapper --gradle-version 8.7`，或把 gradle 放进 PATH。")
    raise SystemExit(1)


def populate_assets(project_root: Path) -> int:
    """把项目内容拷进 Gradle assets。返回拷贝的文件数。"""
    if ASSETS_DEST.exists():
        log(f"清理旧 assets: {ASSETS_DEST}")
        shutil.rmtree(ASSETS_DEST)
    ASSETS_DEST.mkdir(parents=True, exist_ok=True)

    copied = 0
    for src in project_root.rglob("*"):
        if src.is_dir():
            continue
        # 相对路径上任何一级被排除就跳过。
        rel = src.relative_to(project_root)
        if any(part in EXCLUDED_DIR_NAMES for part in rel.parts):
            continue
        if rel.parts[0] == "Build" and (len(rel.parts) < 2 or rel.parts[1] not in KEEP_BUILD_SUBDIRS):
            continue

        dst = ASSETS_DEST / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        copied += 1

    log(f"assets: {copied} 个文件 -> {ASSETS_DEST}")
    if copied == 0:
        fail(f"项目目录里没有任何内容可打进包：{project_root}")
    return copied


def run(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    log("运行: " + " ".join(cmd))
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    result = subprocess.run(cmd, cwd=str(cwd), env=merged_env)
    if result.returncode != 0:
        fail(f"命令失败（退出码 {result.returncode}）: {' '.join(cmd)}")


def locate_output(kind: str, config: str) -> Path:
    """在 Gradle 的输出目录里找产物。

    不要在文件名上猜。这个 Gradle 工程是**单模块根工程**（rootProject.name =
    "ZEngineAndroid"，没有 :app 子目录），AGP 的产物名跟 module name 走，是
    `ZEngineAndroid-<variant>.apk` 还是 `app-<variant>.apk` 取决于 AGP 版本和
    archivesBaseName —— 猜错的后果是 Gradle 明明成功了、脚本却报"没找到产物"，
    而且报错信息指着一个不存在的文件，比不猜更难查。

    所以按扩展名 glob，找不到时把目录里**实际有什么**列出来。
    """
    if kind == "aab":
        out_dir = ANDROID_PROJECT_DIR / "build" / "outputs" / "bundle" / config
        pattern = "*.aab"
    else:
        out_dir = ANDROID_PROJECT_DIR / "build" / "outputs" / "apk" / config
        # output-metadata.json 之类也在里面，只要 .apk。
        pattern = "*.apk"

    if not out_dir.is_dir():
        fail(f"没有输出目录 {out_dir}：Gradle 真的编了吗？")

    found = sorted(out_dir.glob(pattern))
    if not found:
        listing = "\n  ".join(p.name for p in sorted(out_dir.iterdir())) or "(空目录)"
        fail(f"{out_dir} 里没有 {pattern}。目录内容：\n  {listing}")
        raise SystemExit(1)

    if len(found) > 1:
        log(f"输出目录里有 {len(found)} 个 {pattern}，取最新的一个：{[p.name for p in found]}")
        return max(found, key=lambda p: p.stat().st_mtime)

    return found[0]


def adb_install(apk: Path, package: str = "com.zengine.player") -> None:
    adb = None
    sdk = find_android_sdk()
    candidate = sdk / "platform-tools" / "adb"
    if candidate.is_file():
        adb = str(candidate)
    else:
        adb = shutil.which("adb")
    if not adb:
        fail("找不到 adb（SDK 的 platform-tools 或 PATH）。")
        raise SystemExit(1)

    run([adb, "install", "-r", str(apk)], ANDROID_PROJECT_DIR)
    run([adb, "shell", "am", "start", "-n", f"{package}/android.app.NativeActivity"],
        ANDROID_PROJECT_DIR)
    log(f"已安装并启动 {package}")
    log("看日志: adb logcat -s ZEngine")


def main() -> None:
    parser = argparse.ArgumentParser(description="ZEngine Android 出包")
    parser.add_argument("--project", required=True, type=Path,
                        help="项目目录（需含 Build/Android/Bundles 与 .zproject）")
    parser.add_argument("--config", default="debug", choices=["debug", "release"])
    parser.add_argument("--aab", action="store_true", help="打 AAB（上架用）而不是 APK")
    parser.add_argument("--install", action="store_true", help="装到连接的设备并启动")
    parser.add_argument("--skip-assets", action="store_true",
                        help="跳过 assets 填充（复用上一次的内容，加快迭代）")
    parser.add_argument("--gradle-args", default="", help="透传给 gradle 的额外参数")
    args = parser.parse_args()

    project_root: Path = args.project.resolve()
    if not project_root.is_dir():
        fail(f"项目目录不存在: {project_root}")

    if not (project_root / "Build" / "Android").is_dir():
        fail(f"没有找到 {project_root / 'Build' / 'Android'}："
             "先在编辑器里对 Android 平台做一次打包（Build/<Platform>/Bundles）。")

    sdk = find_android_sdk()
    log(f"Android SDK: {sdk}")

    gradle = find_gradle()
    if gradle[0].endswith("gradlew"):
        os.chmod(gradle[0], 0o755)

    if not args.skip_assets:
        populate_assets(project_root)

    task = f"bundle{args.config.capitalize()}" if args.aab else f"assemble{args.config.capitalize()}"
    cmd = gradle + [task]
    if args.gradle_args:
        cmd += args.gradle_args.split()

    env = {"ANDROID_HOME": str(sdk), "ANDROID_SDK_ROOT": str(sdk)}
    run(cmd, ANDROID_PROJECT_DIR, env)

    kind = "aab" if args.aab else "apk"
    output = locate_output(kind, args.config)
    if not output.is_file():
        fail(f"Gradle 跑完了但没找到产物: {output}")
        raise SystemExit(1)

    size_mb = output.stat().st_size / (1024 * 1024)
    log(f"出包完成: {output} ({size_mb:.1f} MB)")

    if args.install:
        if args.aab:
            fail("AAB 不能直接装到设备（要用 bundletool）。改用 --apk 或先转 APKS。")
        adb_install(output)


if __name__ == "__main__":
    main()
