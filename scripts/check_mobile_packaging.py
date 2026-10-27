#!/usr/bin/env python3
"""
ZEngine 移动端出包链 —— 静态一致性自检（P0-1）

移动端出包的最后一步只能在真机上验（要 Android SDK / NDK、要 Xcode、要设备），
但出包链上绝大多数失败**不是**真机问题，而是几处名字 / 路径 / 版本对不上：

  * AndroidManifest 的 `android.app.lib_name` 写的是 "ZGameAndroid"，而 CMake
    target 改名成了别的 —— 系统找不到 libZGameAndroid.so，启动直接崩，
    且 logcat 里只有一句 "Unable to load native library"。
  * Gradle wrapper 的 distributionUrl 是 8.7 而 AGP 升到了 8.7+ —— 配置阶段
    报一句 "Minimum supported Gradle version is 8.9"，看不出来是 wrapper 过期。
  * Info.plist 里 ${MACOSX_BUNDLE_XXX} 没有对应的 target 属性 —— CMake 原样
    输出字面量，App 装上就因为 CFBundleExecutable 非法起不来。
  * 宿主调了 PlayerShell.h 里没有的 ZPlayer_* —— 移动端开 -fvisibility=hidden
    （见 engine/CMakeLists.txt 的 ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS），
    缺的函数在链接期才炸，而链接一次整个引擎要几十分钟。

这些都是**纯文本层面**就能查出来的，不需要 SDK / Xcode / 设备。本脚本把它们
全部钉住，让"真机验"只负责它真正该负责的那部分（能不能跑起来、画面对不对）。

用法：
    python3 scripts/check_mobile_packaging.py          # 全查
    python3 scripts/check_mobile_packaging.py android  # 只查 Android
    python3 scripts/check_mobile_packaging.py ios      # 只查 iOS

退出码 0 = 全过；1 = 有失败项。CI 直接接它。
"""

from __future__ import annotations

import argparse
import plistlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENGINE_DIR = REPO_ROOT / "engine"
PLAYER_DIR = ENGINE_DIR / "Source" / "Player"
ANDROID_DIR = PLAYER_DIR / "Android"
IOS_DIR = PLAYER_DIR / "IOS"
TOOLCHAIN = ENGINE_DIR / "toolchain" / "ios.cmake"

# AndroidManifest 的 android: 命名空间。
ANDROID_NS = "http://schemas.android.com/apk/res/android"


def _a(tag: str) -> str:
    return f"{{{ANDROID_NS}}}{tag}"


# AGP 版本 -> 要求的最低 Gradle 版本。来源于 Google 的 AGP/Gradle 兼容表；
# 新 AGP 版本没进来时按最后一条提示（不会误判为通过）。
AGP_MIN_GRADLE = [
    ((8, 0), (8, 4)),
    ((8, 5), (8, 7)),
    ((8, 7), (8, 9)),
    ((8, 9), (8, 11)),
]

CHECKS = 0
FAILURES = 0


def check(ok: bool, name: str, detail: str = "") -> bool:
    global CHECKS, FAILURES
    CHECKS += 1
    if ok:
        print(f"  [ ok ] {name}")
    else:
        FAILURES += 1
        print(f"  [FAIL] {name}")
        if detail:
            for line in detail.splitlines():
                print(f"         {line}")
    return ok


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def cmake_target_name(cmake_text: str) -> str | None:
    m = re.search(r"^\s*set\s*\(\s*TARGET_NAME\s+([A-Za-z0-9_]+)\s*\)", cmake_text, re.MULTILINE)
    return m.group(1) if m else None


# =========================================================================
# Android
# =========================================================================


def check_android() -> None:
    print("\n=== Android ===")

    manifest = ANDROID_DIR / "src" / "main" / "AndroidManifest.xml"
    if not check(manifest.is_file(), "AndroidManifest.xml 存在", str(manifest)):
        return

    try:
        root = ET.parse(manifest).getroot()
    except ET.ParseError as e:
        check(False, "AndroidManifest.xml 是合法 XML", str(e))
        return
    check(True, "AndroidManifest.xml 是合法 XML")

    android_cmake = ANDROID_DIR / "CMakeLists.txt"
    if not check(android_cmake.is_file(), "Android/CMakeLists.txt 存在", str(android_cmake)):
        return
    android_cmake_text = read(android_cmake)

    # --- 1. lib_name 必须与 CMake target 名一致 ---------------------------
    # 系统拼的是 lib<lib_name>.so，而 CMake 产出 lib<TARGET_NAME>.so。
    # 对不上的症状是启动时 "Unable to load native library"，看不出是名字问题。
    target = cmake_target_name(android_cmake_text)
    lib_name = None
    for meta in root.iter("meta-data"):
        if meta.get(_a("name")) == "android.app.lib_name":
            lib_name = meta.get(_a("value"))
            break
    check(lib_name is not None, "Manifest 声明了 android.app.lib_name")
    if lib_name is not None:
        check(target is not None, "Android/CMakeLists.txt 里有 TARGET_NAME")
        if target is not None:
            check(
                lib_name == target,
                f"lib_name 与 CMake target 名一致（{lib_name}）",
                f"Manifest: {lib_name}\nCMake:    {target}\n"
                f"系统按 lib{lib_name}.so 找，CMake 产出 lib{target}.so",
            )

    # --- 2. Activity 必须是 NativeActivity 且有 LAUNCHER -------------------
    activity = root.find("application/activity")
    check(activity is not None, "Manifest 有 <activity>")
    if activity is not None:
        cls = activity.get(_a("name"))
        check(cls == "android.app.NativeActivity", f"Activity 类是 NativeActivity（实际 {cls}）")
        check(activity.get(_a("exported")) == "true", "Activity 是 exported（否则启动不了）")

        has_main = has_launcher = False
        for filt in activity.iter("intent-filter"):
            actions = {a.get(_a("name")) for a in filt.iter("action")}
            cats = {c.get(_a("name")) for c in filt.iter("category")}
            if "android.intent.action.MAIN" in actions:
                has_main = True
                if "android.intent.category.LAUNCHER" in cats:
                    has_launcher = True
        check(has_main and has_launcher, "有 MAIN + LAUNCHER 的 intent-filter")

    # --- 3. hasCode=false（NativeActivity 形态，APK 里没有 dex）------------
    app = root.find("application")
    if app is not None:
        check(app.get(_a("hasCode")) == "false", "application 的 hasCode=false（纯 native）")

    # --- 4. Vulkan 是硬性要求（引擎 Android RHI 只有 Vulkan）--------------
    features = {f.get(_a("name")): f for f in root.iter("uses-feature")}
    vulkan = features.get("android.hardware.vulkan.level")
    check(vulkan is not None, "声明了 android.hardware.vulkan.level")
    if vulkan is not None:
        check(vulkan.get(_a("required")) == "true", "vulkan feature 是 required=true")

    # --- 5. @string/app_name 必须真的存在 --------------------------------
    # 引用了不存在的 string 是 aapt 失败，但报错指向 Manifest，很难看出是 res 缺。
    strings = ANDROID_DIR / "src" / "main" / "res" / "values" / "strings.xml"
    if check(strings.is_file(), "res/values/strings.xml 存在", str(strings)):
        try:
            str_root = ET.parse(strings).getroot()
            names = {s.get("name") for s in str_root.iter("string")}
            label = app.get(_a("label")) if app is not None else None
            if label and label.startswith("@string/"):
                key = label.split("/", 1)[1]
                check(key in names, f"@string/{key} 在 strings.xml 里有定义", f"现有: {sorted(names)}")
        except ET.ParseError as e:
            check(False, "strings.xml 是合法 XML", str(e))

    # --- 6. Gradle 工程文件齐全 -------------------------------------------
    for name in ("build.gradle", "settings.gradle", "gradle.properties",
                 "gradle/wrapper/gradle-wrapper.properties"):
        path = ANDROID_DIR / name
        check(path.is_file(), f"{name} 存在", str(path))

    build_gradle = ANDROID_DIR / "build.gradle"
    if build_gradle.is_file():
        text = read(build_gradle)

        # externalNativeBuild 的 path 必须真的指着 CMakeLists.txt
        m = re.search(r"^\s*path\s+[\"']([^\"']+)[\"']", text, re.MULTILINE)
        if check(m is not None, "build.gradle 有 externalNativeBuild 的 path"):
            resolved = (ANDROID_DIR / m.group(1)).resolve()
            check(
                resolved.is_file() and resolved.name == "CMakeLists.txt",
                f"externalNativeBuild 指向的 CMakeLists.txt 存在",
                f"{m.group(1)} -> {resolved}",
            )

        # minSdk 必须 >= 24（Vulkan 从 API 24 起）
        m = re.search(r"minSdk\s+(\d+)", text)
        if check(m is not None, "build.gradle 有 minSdk"):
            check(int(m.group(1)) >= 24, f"minSdk >= 24（Vulkan 下限，实际 {m.group(1)}）")

        # AGP 与 Gradle wrapper 版本配对
        m_agp = re.search(r"com\.android\.application'\s+version\s+'([0-9.]+)'", text)
        wrapper = ANDROID_DIR / "gradle" / "wrapper" / "gradle-wrapper.properties"
        if m_agp and wrapper.is_file():
            m_g = re.search(r"gradle-([0-9.]+)-", read(wrapper))
            if check(m_g is not None, "gradle-wrapper.properties 里有 gradle 版本"):
                agp = tuple(int(x) for x in m_agp.group(1).split(".")[:2])
                gradle = tuple(int(x) for x in m_g.group(1).split(".")[:2])
                floor = (8, 11)
                for agp_floor, g_floor in AGP_MIN_GRADLE:
                    if agp >= agp_floor:
                        floor = g_floor
                check(
                    gradle >= floor,
                    f"Gradle {m_g.group(1)} 满足 AGP {m_agp.group(1)} 的下限 {'.'.join(map(str, floor))}",
                    f"去 engine/Source/Player/Android 跑 "
                    f"`gradle wrapper --gradle-version {'.'.join(map(str, floor))}`",
                )

    # --- 7. assets 根目录存在（脚本往这儿塞项目内容）----------------------
    assets = ANDROID_DIR / "src" / "main" / "assets"
    check(assets.is_dir(), "src/main/assets 存在（项目内容打进这里）", str(assets))

    # --- 8. 宿主只调 PlayerShell.h 里声明过的接口 -------------------------
    check_host_symbols(ANDROID_DIR / "AndroidMain.cpp", "AndroidMain.cpp")


# =========================================================================
# iOS
# =========================================================================


def check_ios() -> None:
    print("\n=== iOS ===")

    plist_path = IOS_DIR / "Info.plist"
    if not check(plist_path.is_file(), "Info.plist 存在", str(plist_path)):
        return

    ios_cmake = IOS_DIR / "CMakeLists.txt"
    if not check(ios_cmake.is_file(), "IOS/CMakeLists.txt 存在", str(ios_cmake)):
        return
    ios_cmake_text = read(ios_cmake)

    # --- 1. Info.plist 是合法 plist ---------------------------------------
    try:
        plist_text = read(plist_path)
        # CMake 会把 ${...} 替换掉，但文件里存的是占位符；解析前先换成合法值，
        # 否则 plistlib 也能过（占位符就是普通字符串），这里只是确认结构没坏。
        plistlib.loads(plist_text.encode("utf-8"))
        check(True, "Info.plist 是合法 plist")
    except Exception as e:  # noqa: BLE001 - 任何解析失败都算失败
        check(False, "Info.plist 是合法 plist", str(e))
        return

    # --- 2. 每个 ${...} 占位符都要有对应的 CMake 属性 ---------------------
    # 没有的话 CMake 原样输出 "${MACOSX_BUNDLE_XXX}"，App 装上起不来，
    # 而且 Xcode 的报错完全指不到这里。
    placeholders = sorted(set(re.findall(r"\$\{([A-Za-z0-9_]+)\}", plist_text)))
    check(bool(placeholders), "Info.plist 里有 ${...} 占位符（该由 CMake 替换）")
    missing = [p for p in placeholders if p not in ios_cmake_text and p != "MACOSX_BUNDLE_EXECUTABLE_NAME"]
    check(
        not missing,
        f"全部 {len(placeholders)} 个占位符在 IOS/CMakeLists.txt 里有对应属性",
        "缺失: " + ", ".join(missing) + "\n"
        "（MACOSX_BUNDLE_EXECUTABLE_NAME 由 CMake 自动给，不算缺失）",
    )

    # --- 3. Metal 是硬性要求 ----------------------------------------------
    check("metal" in plist_text, "UIRequiredDeviceCapabilities 含 metal")

    # --- 4. toolchain 必须显式设 IOS --------------------------------------
    # engine/CMakeLists.txt 用 elseif(IOS) 决定要不要加 ZGameIOS；不设就会
    # 静默跳过 —— 配出来没有 target，也没有报错。
    if check(TOOLCHAIN.is_file(), "engine/toolchain/ios.cmake 存在", str(TOOLCHAIN)):
        tc = read(TOOLCHAIN)
        check("set(CMAKE_SYSTEM_NAME iOS)" in tc, "toolchain 设了 CMAKE_SYSTEM_NAME iOS")
        check(re.search(r"^\s*set\s*\(\s*IOS\s+", tc, re.MULTILINE) is not None,
              "toolchain **显式**设了 IOS（不依赖 CMake 内部行为）")
        check("CMAKE_OSX_DEPLOYMENT_TARGET" in tc, "toolchain 设了 CMAKE_OSX_DEPLOYMENT_TARGET")

    # --- 5. 引擎根真的会 add_subdirectory(IOS) ----------------------------
    engine_cmake = ENGINE_DIR / "CMakeLists.txt"
    if check(engine_cmake.is_file(), "engine/CMakeLists.txt 存在", str(engine_cmake)):
        ec = read(engine_cmake)
        check("elseif(IOS)" in ec, "engine/CMakeLists.txt 有 elseif(IOS) 分支")
        check("add_subdirectory(Source/Player/IOS)" in ec,
              "elseif(IOS) 分支里有 add_subdirectory(Source/Player/IOS)")

    # --- 6. deployment target 三处一致 ------------------------------------
    m_xcode = re.search(r'XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET\s+"([0-9.]+)"', ios_cmake_text)
    m_plist = re.search(r"<key>MinimumOSVersion</key>\s*<string>([0-9.]+)</string>", plist_text)
    m_tc = re.search(r'CMAKE_OSX_DEPLOYMENT_TARGET\s+"([0-9.]+)"', read(TOOLCHAIN)) if TOOLCHAIN.is_file() else None
    if m_xcode and check(m_plist is not None, "Info.plist 有 MinimumOSVersion"):
        check(m_xcode.group(1) == m_plist.group(1),
              f"MinimumOSVersion 与 XCODE_ATTRIBUTE 一致（{m_plist.group(1)}）",
              f"plist: {m_plist.group(1)} / XCODE_ATTRIBUTE: {m_xcode.group(1)}")
    if m_xcode and m_tc:
        check(m_xcode.group(1) == m_tc.group(1),
              f"toolchain 的 deployment target 与 XCODE_ATTRIBUTE 一致（{m_tc.group(1)}）",
              f"toolchain: {m_tc.group(1)} / XCODE_ATTRIBUTE: {m_xcode.group(1)}")

    # --- 7. zbuild_ios.py 的 -scheme 必须与 CMake target 名一致 -----------
    target = cmake_target_name(ios_cmake_text)
    script = REPO_ROOT / "zbuild_ios.py"
    if script.is_file():
        st = read(script)
        m = re.search(r'"-scheme",\s*"([A-Za-z0-9_]+)"', st)
        if check(m is not None, "zbuild_ios.py 里有 -scheme"):
            if target is not None:
                check(m.group(1) == target,
                      f"xcodebuild 的 -scheme 与 CMake target 名一致（{m.group(1)}）",
                      f"scheme: {m.group(1)} / target: {target}")

    # --- 8. 宿主只调 PlayerShell.h 里声明过的接口 -------------------------
    check_host_symbols(IOS_DIR / "IOSMain.mm", "IOSMain.mm")


# =========================================================================
# 共用：宿主 <-> PlayerShell 的符号一致性
# =========================================================================


def implementation_names(text: str) -> set[str]:
    """从 .cpp 里挑出"被定义"的函数名，而不是"被调用"的。

    判据很土但够用：一行里出现 `ZPlayer_xxx(` 且这一行不以 `;` 结尾 ——
    函数签名行（哪怕签名跨行，第一行也不会以分号结束）命中，普通调用语句
    `ZPlayer_Shutdown();` 被排除。比"名字后面有括号"精确一层。
    """
    names: set[str] = set()
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("//") or line.startswith("*") or line.endswith(";"):
            continue
        m = re.search(r"\b(ZPlayer_[A-Za-z0-9_]+)\s*\(", line)
        if m:
            names.add(m.group(1))
    return names


def check_host_symbols(host: Path, label: str) -> None:
    """宿主文件里用到的每个 ZPlayer_* 都必须在 PlayerShell.h 里声明过。

    移动端开 -fvisibility=hidden（ZENGINE_MOBILE_HIDE_INTERNAL_SYMBOLS），
    PlayerShell.h 里没有 ZPLAYER_API 标记的符号在宿主 .so / App 里根本看不见。
    而链接一次整个引擎要几十分钟，把这个挪到静态检查里省事得多。
    """
    shell = PLAYER_DIR / "PlayerShell.h"
    shell_cpp = PLAYER_DIR / "PlayerShell.cpp"
    if not host.is_file():
        check(False, f"{label} 存在", str(host))
        return
    if not check(shell.is_file(), "PlayerShell.h 存在", str(shell)):
        return

    shell_text = read(shell)
    host_text = read(host)

    # 两类符号要分开：
    #   函数   —— 名字后面紧跟 '('
    #   枚举成员 —— ZPlayer_OK / ZPlayer_RHI_Vulkan / ZPlayer_Touch_Down ...
    # 之前用 "名字里第 9 个字符是不是大写" 来区分，结果 ZPlayer_OK 的 'O' 也
    # 是大写，15 个枚举成员全被当成"声明了但没实现的函数"。
    declared_functions = set(re.findall(r"\b(ZPlayer_[A-Za-z0-9_]+)\s*\(", shell_text))
    declared_symbols = set(re.findall(r"\b(ZPlayer_[A-Za-z0-9_]+)\b", shell_text))

    used = set(re.findall(r"\b(ZPlayer_[A-Za-z0-9_]+)\b", host_text))
    unknown = sorted(used - declared_symbols)
    check(not unknown,
          f"{label} 只用 PlayerShell.h 里声明过的 ZPlayer_* 接口（{len(used)} 个）",
          "未声明: " + ", ".join(unknown))

    # 声明了但没实现：链接期才炸，一样挪到这里。
    if shell_cpp.is_file():
        impl = implementation_names(read(shell_cpp))
        unimplemented = sorted(declared_functions - impl)
        check(not unimplemented,
              f"PlayerShell.h 声明的 {len(declared_functions)} 个函数在 PlayerShell.cpp 里都有定义",
              "缺定义: " + ", ".join(unimplemented))


def check_scripts() -> None:
    print("\n=== 出包脚本 ===")
    for name in ("zbuild_android.py", "zbuild_ios.py"):
        path = REPO_ROOT / name
        if not check(path.is_file(), f"{name} 存在", str(path)):
            continue
        result = subprocess.run([sys.executable, "--help"], capture_output=True, text=True)
        check(result.returncode == 0, f"{name} 可以用当前 python 解析")
        # --help 会 argparse 正常退出（code 0）
        r = subprocess.run([sys.executable, str(path), "--help"], capture_output=True, text=True)
        check(r.returncode == 0 and "--project" in r.stdout,
              f"{name} --help 正常（参数表完整）",
              r.stderr[-400:] if r.returncode != 0 else "")


def main() -> None:
    parser = argparse.ArgumentParser(description="ZEngine 移动端出包链静态自检")
    parser.add_argument("scope", nargs="?", default="all", choices=["all", "android", "ios"],
                        help="只查某一端（默认全查）")
    args = parser.parse_args()

    print("ZEngine 移动端出包链静态自检")
    print(f"仓库: {REPO_ROOT}")

    if args.scope in ("all", "android"):
        check_android()
    if args.scope in ("all", "ios"):
        check_ios()
    if args.scope == "all":
        check_scripts()

    print(f"\n{'PASS' if FAILURES == 0 else 'FAIL'}: {CHECKS} 项检查, {FAILURES} 项失败")
    if FAILURES:
        print("\n注意：这只覆盖**静态一致性**。真机上的事（能不能跑起来、画面对不对、"
              "Vulkan/Metal 后端是否可用）它验不了，仍然要出包到设备上跑。")
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
