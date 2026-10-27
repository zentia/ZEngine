#!/usr/bin/env python3
"""在没有 cmake / ninja 的机器上对**任意**编译单元做 `-fsyntax-only`。

为什么需要它
------------
`scripts/run_smoke_test.py` 只能"现编一个测试并真跑"，引擎侧改动本身它验证
不到。而 Editor 模块（ZEditor）在本机**根本没有可用的 .a**（bin/ 里只有
Runtime 那几个），所以 Editor 的 TU 连链接都做不到 —— 唯一能做的静态检查就
是 `-fsyntax-only`。这条链路是 2026-10-27 改 Prefab Variant 时摸出来的。

做法：复用 `build/compile_commands.json` 里该 TU 的**完整 flags**，不做任何
猜测。关键是必须剥掉**二进制 PCH**：

    -Winvalid-pch
    -Xclang -include-pch -Xclang <...cmake_pch.hxx.pch>

`build/` 里那份 .pch 是上一次 cmake 构建的产物，早过期了。留着它会报

    fatal error: file '.../Type.h' has been modified since the precompiled
    header '...cmake_pch.hxx.pch' was built
    error: redefinition of 'TypeFlags'

剥掉之后**保留** `-Xclang -include -Xclang <cmake_pch.hxx>`（那是把 PCH 的
头文件当普通 force-include 塞进去，不是二进制 PCH），编译就干净了。

用法
----
    python3 scripts/zsyntax.py engine/Source/Editor/Prefab/PrefabUtilityEditor.cpp
    python3 scripts/zsyntax.py engine/Source/Runtime/Resource/Prefab/*.cpp

    # 该文件不在 compile_commands.json 里时，借另一条的 flags（按子串匹配）
    python3 scripts/zsyntax.py <file> --template Function/Audio/AudioSystem.cpp

退出码：全部通过 0，任一失败 1。
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = REPO_ROOT / "build" / "compile_commands.json"


def strip_pch(args: list[str]) -> list[str]:
    """去掉 -o <out> 和二进制 PCH 参数；保留 cmake_pch.hxx 的 force-include。"""
    out: list[str] = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-Winvalid-pch":
            i += 1
            continue
        if (
            a == "-Xclang"
            and i + 3 < len(args)
            and args[i + 1] == "-include-pch"
            and args[i + 2] == "-Xclang"
            and args[i + 3].endswith(".pch")
        ):
            i += 4
            continue
        if a == "-o":
            i += 2
            continue
        out.append(a)
        i += 1
    return out


def find_entry(db: list[dict], file: Path, template: str | None) -> dict | None:
    target = str(file)
    for e in db:
        if e["file"] == target or e["file"].endswith(target):
            return e
    # 退而求其次：按 basename
    for e in db:
        if Path(e["file"]).name == file.name:
            return e
    if template:
        for e in db:
            if template in e["file"]:
                return e
    return None


def check(db: list[dict], file: Path, template: str | None, verbose: bool) -> bool:
    entry = find_entry(db, file, template)
    if entry is None:
        print(f"  [skip] {file.name}: not in compile_commands.json")
        return True

    import shlex

    args = strip_pch(shlex.split(entry["command"]))
    # 用目标文件替换编译命令里的源文件（template 模式下必须）
    args = [file.resolve().as_posix() if a == entry["file"] else a for a in args]
    args.append("-fsyntax-only")

    r = subprocess.run(args, cwd=entry["directory"], capture_output=True, text=True)
    if r.returncode == 0:
        print(f"  [ ok ] {file.name}")
        return True

    print(f"  [FAIL] {file.name}")
    err = r.stderr or ""
    if verbose:
        print(err)
    else:
        for line in err.splitlines():
            if "error:" in line or "fatal error:" in line:
                print("         " + line.strip())
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="要检查的 .cpp（也接受 glob，由 shell 展开）")
    ap.add_argument("--db", default=str(DEFAULT_DB))
    ap.add_argument("--template", default=None, help="CDB 里借 flags 用的条目子串")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"missing {db_path}", file=sys.stderr)
        return 1
    db = json.loads(db_path.read_text())

    ok = True
    for f in args.files:
        ok &= check(db, Path(f), args.template, args.verbose)
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
