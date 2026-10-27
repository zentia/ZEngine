#!/usr/bin/env python3
"""在没有 cmake 的机器上真正 build + run 一个冒烟测试。

为什么需要它
------------
zsyntax.py 只做 `-fsyntax-only`。也就是说在这台没有 cmake / ninja 的机器上，
之前所有改动的最强验证只是"编译得过"，**没有任何一行引擎代码被真正执行过**。
脚本绑定层累计到 70 个类 / 265 个方法时这一点变得很刺眼。

这个脚本补上最后一环：复用 `build/compile_commands.json` 里的一条编译命令拿
到全部 flags，把测试编成可执行文件，并**链到已有的 `bin/libZRuntime.a`**，然后
真的跑它、把退出码透出来。

关键取舍：只编译测试自己的 .cpp，引擎侧的实现从 `libZRuntime.a` 拉。
试过把 Vector3.cpp / Quaternion.cpp 一起编进来，结果 Transfer() 把序列化、
日志、mimalloc、EASTL allocator 一路拖成一片 undefined symbols —— 越补越多。
.a 是上一次 cmake 构建的产物（会过期），但它提供的正是这些稳定的底层实现；
被测的新代码是本脚本现编的，所以"过期"不影响验证新逻辑。

用法
----
    python3 scripts/run_smoke_test.py \
        engine/Source/Runtime/Scripting/ScriptMath/Test/ScriptMathSmokeTest.cpp

    # 换一条编译命令当模板（默认取 Function/Audio/AudioSystem.cpp）
    python3 scripts/run_smoke_test.py <test.cpp> --template Function/Render/RenderScene.cpp

退出码 = 测试的退出码（0 = 全部检查通过）。

本机各冒烟测试需要的 --extra-source
----------------------------------
`bin/libZRuntime.a` 比最近几次提交都旧（2026-10-26），凡是被那几次提交**改过**
的引擎源文件都得显式列进来，否则链接期看不出来（Apple 上有
`-Wl,-undefined,dynamic_lookup`），运行期一跳进去就是 SIGSEGV：

    ScriptTime    --extra-source engine/Source/Runtime/Core/Time/TimeState.cpp
    ScriptInput   --extra-source engine/Source/Runtime/Function/Input/EnhancedInputSystem.cpp
                  --extra-source engine/Source/Runtime/Function/Input/InputSystem.cpp
    ScriptVM      见 doc/SCRIPT_VM_E2E.md 第 4 节（还要额外 4 个 .gen.cpp）

症状很好认：BqLog 打完 "log_manager is constructed" 就没了，`echo $?` 是 139。
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CDB = REPO_ROOT / "build" / "compile_commands.json"
BIN_DIR = REPO_ROOT / "bin"

# 链接时按需要提供的第三方静态库。只列存在的，缺了就让链接器自己报。
LINK_LIBS = [
    "ZRuntime",
    "mimalloc",
    "spirv-cross-core",
    "spirv-cross-glsl",
    "astcenc",
    "bc7enc",
    "lmdb",
    # 拖到脚本层 / 资源层时才会被需要的几个：pesapi 的实现、zlib、puerts
    # 核心。libZRuntime.a 里的 ScriptEnv.cpp.o 引用 pesapi_*，
    # AssetBundleFormat.cpp.o 引用 uncompress，少了它们就是一大片
    # undefined symbols。只列存在的，不影响链不到的测试。
    "PapiQuickjs",
    "PuertsCore",
    "z",
    # 再往下（typeid(WindowSystem) 顺藤摸瓜拉进来的物理 / 资源层）：
    "Jolt",
    "minizip-ng",
    "xxhash",
    "ufbx",
    "tinyobjloader",
    "curl",
    "ZSlate",
    "ShaderLab",
]


def build_command(template_fragment: str) -> list[str]:
    if not CDB.is_file():
        raise SystemExit(f"找不到 {CDB}：这台机器没跑过 cmake，拿不到编译 flags。")

    db = json.loads(CDB.read_text(encoding="utf-8"))
    matches = [e for e in db if template_fragment in e["file"]]
    if not matches:
        raise SystemExit(f"compile_commands.json 里没有匹配 '{template_fragment}' 的条目。")
    args = shlex.split(matches[0]["command"])

    out: list[str] = []
    skip_next = False
    i = 0
    while i < len(args):
        a = args[i]
        # 整段丢掉 PCH：`-Xclang -include-pch -Xclang <path>`。逐个 token 过滤会
        # 把后面的 -Xclang 当成"PCH 路径占位符"替换掉，clang 于是报
        # "cannot specify -o when generating multiple output files"。
        if a == "-Xclang" and i + 3 < len(args) and args[i + 1] == "-include-pch" and args[i + 2] == "-Xclang":
            i += 4
            continue
        i += 1
        if skip_next:
            skip_next = False
            continue
        if a == "-o":
            skip_next = True
            continue
        if a == "-c":
            # compile_commands 里 -c 是"编译不链接"，配 -o 会冲突。
            continue
        if a == "-fsyntax-only":
            continue
        if a.endswith(".cpp") or a.endswith(".cc") or a.endswith(".mm") or a.endswith(".pch"):
            continue
        out.append(a)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="无 cmake 也能 build + run 冒烟测试")
    parser.add_argument("test_source", help="冒烟测试的 .cpp（相对仓库根或绝对路径）")
    parser.add_argument("--template", default="Function/Audio/AudioSystem.cpp",
                        help="从 compile_commands.json 里挑哪一条命令当 flags 模板")
    parser.add_argument("--extra-source", action="append", default=[], metavar="CPP",
                        help="除测试自身外还要一起编译的 .cpp，可重复。用于**新加的引擎源文件**："
                             "bin/libZRuntime.a 是上一次 cmake 构建的产物，不含它们的符号，"
                             "不一起编就是 undefined symbol。")
    args = parser.parse_args()

    test_path = Path(args.test_source)
    if not test_path.is_absolute():
        test_path = REPO_ROOT / test_path
    if not test_path.is_file():
        raise SystemExit(f"测试文件不存在：{test_path}")

    out_bin = f"/tmp/{test_path.stem}"

    extra_paths: list[Path] = []
    for raw in args.extra_source:
        p = Path(raw)
        if not p.is_absolute():
            p = REPO_ROOT / p
        if not p.is_file():
            raise SystemExit(f"--extra-source 文件不存在：{p}")
        extra_paths.append(p)

    cmd = build_command(args.template)
    # ZHeaderTool 的 *.generated.h 是构建产物，不入库；本脚本自己生成一份放 /tmp。
    cmd.append("-I/tmp/zgen_sa/inc")
    cmd += ["-o", out_bin, str(test_path)]
    cmd += [str(p) for p in extra_paths]
    cmd.append(f"-L{BIN_DIR}")
    cmd.append(f"-F{BIN_DIR}")
    # 静态库顺序敏感（依赖者在前、被依赖者在后），而我们无从知道真实依赖
    # 图 —— 列两遍是链接静态库时的常规兜底。
    for _ in range(2):
        for lib in LINK_LIBS:
            if (BIN_DIR / f"lib{lib}.a").is_file():
                cmd.append(f"-l{lib}")
    if (BIN_DIR / "BqLog.framework").exists():
        cmd += ["-framework", "BqLog"]

    # Apple：只要链接器顺着某个 typeinfo（例如 typeid(WindowSystem)）拉进了
    # MacOSWindow.mm.o，就会一路要 Objective-C 运行时、Cocoa、乃至 Jolt 物理
    # —— 越补越多（补到第四层还没完）。
    #
    # 所以这里改成让未定义符号延迟到运行时解析：libZRuntime.a 是一个完整的
    # 构建产物，缺的只是**测试不会走到的**那些分支依赖的第三方库。测试只要
    # 不调用它们就不会解析，不会崩。真调用到了会 dyld 报错而不是静默出错。
    if sys.platform == "darwin":
        cmd.append("-Wl,-undefined,dynamic_lookup")
        for fw in ("Cocoa", "QuartzCore", "Metal", "IOKit", "CoreVideo", "CoreGraphics"):
            cmd += ["-framework", fw]

    print(f"[build] {test_path.name} ...", flush=True)
    r = subprocess.run(cmd, cwd=REPO_ROOT / "build", capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0:
        return r.returncode

    print(f"\n[run] {out_bin}", flush=True)
    rr = subprocess.run([out_bin], capture_output=True, text=True)
    sys.stdout.write(rr.stdout)
    sys.stderr.write(rr.stderr)
    return rr.returncode


if __name__ == "__main__":
    sys.exit(main())
