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
    args = parser.parse_args()

    test_path = Path(args.test_source)
    if not test_path.is_absolute():
        test_path = REPO_ROOT / test_path
    if not test_path.is_file():
        raise SystemExit(f"测试文件不存在：{test_path}")

    out_bin = f"/tmp/{test_path.stem}"

    cmd = build_command(args.template)
    # ZHeaderTool 的 *.generated.h 是构建产物，不入库；本脚本自己生成一份放 /tmp。
    cmd.append("-I/tmp/zgen_sa/inc")
    cmd += ["-o", out_bin, str(test_path)]
    cmd.append(f"-L{BIN_DIR}")
    cmd.append(f"-F{BIN_DIR}")
    for lib in LINK_LIBS:
        if (BIN_DIR / f"lib{lib}.a").is_file():
            cmd.append(f"-l{lib}")
    if (BIN_DIR / "BqLog.framework").exists():
        cmd += ["-framework", "BqLog"]

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
