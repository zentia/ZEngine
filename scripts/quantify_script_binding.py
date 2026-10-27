#!/usr/bin/env python3
"""量化脚本绑定覆盖度：ZFUNCTION -> pesapi 的真实当前状态。

为什么需要这个脚本
------------------
`bin/Debug/Intermediate/Typings/zengine.d.ts` 和 `build/Generated/ZHT/**/*.gen.cpp`
都是**构建产物**。没有 cmake 的机器上重新生成不了，于是它们会停在某一时刻；照着
一份过期的 d.ts 判断"某个 API 有没有导出"会得出完全错误的结论（我据此下过一次
错误判断：以为 GameObject 没有 AddComponent，实际它有，只是产物是 7 天前的）。

本脚本直接跑 ZHeaderTool 拿**当前**状态，不需要 cmake / 编译：
    * 跑 `ZHeaderTool.py generate --dump-ir` 导出 IR（纯 Python，能直接跑）；
    * 生成阶段本身就会校验 TypeScript 类型映射（缺映射会直接报错）；
    * 再用 IR 里每个方法的签名去对照 ZScript::Converter 的特化，找出运行时
      转不动的类型。

注意两套类型是**独立**的，都要过：
    * `ZScript::Converter<T>`（engine/.../Scripting/Binding/ScriptBinding.h）
      负责运行时 C++ <-> JS 的值转换；
    * `zht/parser.py` 的 `_TS_TYPES` 负责 .d.ts 的拼写。
只加其中一个，另一个会在各自阶段报错。

用法
----
    python3 scripts/quantify_script_binding.py                 # 全量报告
    python3 scripts/quantify_script_binding.py --class GameObject
    python3 scripts/quantify_script_binding.py --missing-only

退出码 0 = 没有缺 Converter 的签名；1 = 有。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ZHT = REPO_ROOT / "engine" / "tools" / "ZHeaderTool" / "ZHeaderTool.py"
SCRIPT_BINDING_H = (
    REPO_ROOT / "engine" / "Source" / "Runtime" / "Scripting" / "Binding" / "ScriptBinding.h"
)

# 模板特化覆盖不到的"泛化"规则，判定时要一并算进去：
#   is_integral<T> && !is_same<T,bool>  -> 所有整型
#   is_enum<T>                          -> 所有 ZENUM
#   T* / T& where T derives Object      -> 所有绑定类
INTEGRALS = {
    "char", "signed char", "unsigned char", "short", "unsigned short",
    "int", "unsigned", "unsigned int", "long", "unsigned long", "long long",
    "unsigned long long", "int8_t", "uint8_t", "int16_t", "uint16_t",
    "int32_t", "uint32_t", "int64_t", "uint64_t", "size_t", "std::size_t",
}
SCALARS = INTEGRALS | {"bool", "float", "double"}


def strip_cv_ref(cpp_type: str) -> str:
    """`const eastl::string&` -> `eastl::string`。对齐 ts_type_for() 的规范化。"""
    text = " ".join(cpp_type.split())
    while text.startswith("const "):
        text = text[len("const "):].strip()
    while text.endswith("&&") or text.endswith("&"):
        text = text[:-2].strip() if text.endswith("&&") else text[:-1].strip()
    if text.endswith("*"):
        text = text[:-1].strip()
        while text.startswith("const "):
            text = text[len("const "):].strip()
    return text


def converter_types() -> set[str]:
    """从 ScriptBinding.h 里读出所有显式特化的 Converter 类型名。

    用解析而不是硬编码一份名单：名单会漂移，漂移了就又变成一份过期的东西。
    """
    text = SCRIPT_BINDING_H.read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r"struct\s+Converter<\s*([^>]+?)\s*>", text))


def typedef_aliases(source_dir: Path) -> dict[str, str]:
    """扫出 `using X = 基础类型;` / `typedef 基础类型 X;`。

    GObjectID 是 uint64_t 的别名、AudioVoiceId 也是 —— 名字对不上任何 Converter
    特化，但底层类型是整型，会命中 is_integral 的泛化。不展开别名就会把它们
    报成"缺 Converter"（这两个恰好是我在第一次量化时误判过的两个）。

    从源码扫而不是列名单，理由同 converter_types()。
    """
    aliases: dict[str, str] = {}
    for path in source_dir.rglob("*.h"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, base in re.findall(r"\busing\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_][\w:]*)\s*;", text):
            aliases.setdefault(name, base)
        for base, name in re.findall(r"\btypedef\s+([A-Za-z_][\w:]*)\s+([A-Za-z_]\w*)\s*;", text):
            aliases.setdefault(name, base)
    return aliases


def resolve_alias(name: str, aliases: dict[str, str]) -> str:
    """展开 typedef 链（带环保护）。"""
    seen = {name}
    while name in aliases:
        name = aliases[name]
        if name in seen:
            break
        seen.add(name)
    return name


def collect_headers(source_dir: Path) -> list[str]:
    files = sorted(str(p) for p in source_dir.rglob("*.h"))
    if not files:
        raise SystemExit(f"{source_dir} 下没有 .h")
    return files


def run_zht(headers: list[str], out_dir: Path) -> dict:
    headers_file = out_dir / "headers.txt"
    headers_file.write_text("\n".join(headers) + "\n", encoding="utf-8")
    manifest = out_dir / "annotated.txt"
    subprocess.run(
        [sys.executable, str(ZHT), "list",
         "--headers-file", str(headers_file), "--output", str(manifest)],
        check=True, capture_output=True, text=True,
    )
    ir_path = out_dir / "ir.json"
    # generate 会顺带校验 TypeScript 类型映射：缺 _TS_TYPES 条目时这里直接失败。
    result = subprocess.run(
        [sys.executable, str(ZHT), "generate",
         "--module", "ZRuntime",
         "--headers-file", str(headers_file),
         "--include-root", str(REPO_ROOT / "engine" / "Source"),
         "--out-inc", str(out_dir / "Inc"),
         "--out-src", str(out_dir / "Src"),
         "--manifest", str(manifest),
         "--stamp", str(out_dir / "zht.stamp"),
         "--dump-ir", str(ir_path)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print("ZHeaderTool generate 失败（多半是缺 TypeScript 类型映射）：", file=sys.stderr)
        print(result.stdout[-4000:], file=sys.stderr)
        print(result.stderr[-2000:], file=sys.stderr)
        raise SystemExit(1)
    return json.loads(ir_path.read_text(encoding="utf-8"))


def main() -> None:
    ap = argparse.ArgumentParser(description="量化 ZEngine 脚本绑定覆盖度")
    ap.add_argument("--source-dir", default=str(REPO_ROOT / "engine" / "Source" / "Runtime"),
                    help="扫哪些头文件（默认 engine/Source/Runtime）")
    ap.add_argument("--class", dest="only_class", help="只看某一个类的方法")
    ap.add_argument("--missing-only", action="store_true", help="只打印缺 Converter 的部分")
    args = ap.parse_args()

    ir = run_zht(collect_headers(Path(args.source_dir)), Path(tempfile.mkdtemp(prefix="zht-quantify-")))

    classes = [c for f in ir for c in f.get("classes", [])]
    enums = {e.get("name") for f in ir for e in f.get("enums", [])}
    class_names = {c["name"] for c in classes}
    conv = converter_types()
    aliases = typedef_aliases(Path(args.source_dir))

    missing: Counter[str] = Counter()
    where: dict[str, list[str]] = defaultdict(list)
    total = 0
    statics = 0

    for c in classes:
        for m in c.get("methods", []):
            total += 1
            if m.get("is_static"):
                statics += 1
            for t in [(m.get("return_type") or "void")] + [p["cpp_type"] for p in m.get("params", [])]:
                base = strip_cv_ref(t)
                if base == "void":           # 返回类型，不参与转换
                    continue
                base = resolve_alias(base, aliases)
                if base in SCALARS or base in conv:
                    continue
                if base in enums:            # is_enum 泛化
                    continue
                if base in class_names:      # T* (T : Object) 泛化
                    continue
                missing[base] += 1
                where[base].append(f"{c['name']}::{m.get('script_name') or m.get('name')}")

    if args.only_class:
        hit = [c for c in classes if c["name"] == args.only_class]
        if not hit:
            raise SystemExit(f"没有类 {args.only_class}")
        print(f"=== {args.only_class} ===")
        for m in hit[0].get("methods", []):
            print(f"  {m.get('ts_signature')}"
                  f"{'  [static]' if m.get('is_static') else ''}")
        return

    print(f"类 {len(classes)} | 方法 {total} | 静态方法 {statics}")
    print(f"缺 Converter 的类型种类 {len(missing)}")
    if missing and not args.missing_only:
        print()
    for t, n in missing.most_common():
        print(f"  {n:4d}  {t}")
        print(f"        例: {', '.join(where[t][:3])}")

    if missing:
        print("\n这些类型在 ScriptBinding.h 里没有 Converter 特化，"
              "也不是整型 / enum / Object 派生指针（后三类由泛化规则覆盖）。")
        sys.exit(1)
    if not args.missing_only:
        print("\n全部签名都有 Converter。")


if __name__ == "__main__":
    main()
