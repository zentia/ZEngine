#!/usr/bin/env python3
"""SetActive 的不变量自检。

背景（doc/SCRIPT_SCENE_API.md §4）：GameObject 的激活状态存在 `Transform::m_Enabled`
里（Component::m_Enabled），`GameObject::SetActive` 是**唯一**写入口 —— 因为它除了写
flag 还要做两件别人做不到的事：

  1. 通知组件（`Component::OnOwnerActiveChanged`）。对象停用后 `GameObject::Tick`
     直接 return，组件的 Tick 再也跑不到，没有任何一次 Tick 能替它们收尾（撤渲染
     实体、关骨架可见性）。
  2. 沿 Transform 子树递归重推。父级开关一变，整棵子树的 activeInHierarchy 都变了，
     但后代自己的 activeSelf 没变。

所以"某个地方直接 `transform->SetEnabled(...)`"就是 bug：值改了，上面两件都没做。
这个脚本就是把这条不变量钉住 —— 之前有三处犯过（Timeline / Inspector / 撤销）。

判据是**纯文本**的，不解析 C++：直接写 Transform 的 enabled 只有那几种变量名拼写
（`transform->` / `xf->` …），而合法调用点要么走 `->SetActive(`，要么是子类 override
里的 `Component::SetEnabled(enabled)` 转调。为了不变成"永远通过"的检查，脚本自带
`--selftest`：把旧的三处违规写法注入临时副本，确认检查**会**失败。

用法：
    python3 scripts/check_active_api.py              # 检查
    python3 scripts/check_active_api.py --selftest   # 负向验证（确认检查不是摆设）
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPO_ROOT / "engine" / "Source"
RUNTIME_ROOT = SOURCE_ROOT / "Runtime"
EDITOR_ROOT = SOURCE_ROOT / "Editor"

SCAN_ROOTS = [RUNTIME_ROOT, EDITOR_ROOT]
SCAN_SUFFIXES = {".h", ".cpp"}

# 直接写 Transform 的 enabled。`->` 而不是 `.`：Transform 从来不是值成员。
# 变量名来自这次实际犯过事的三处（transform / xf）+ 几个常见短名。
DIRECT_TRANSFORM_SETENABLED = re.compile(
    r"\b(?:transform|xf|t|tr|self_transform|parent_transform|child_transform)->SetEnabled\s*\("
)

# 消费方必须问层级状态（activeInHierarchy），而不是只问自己的 Transform。
# 路径相对 SOURCE_ROOT。
CONSUMERS: list[tuple[str, str]] = [
    ("Tick 门禁", "Runtime/BaseClasses/GameObject.cpp", "IsActiveInHierarchy"),
    ("网格渲染提交", "Runtime/Function/Framework/Component/Mesh/BaseRenderer.cpp", "IsActiveInHierarchy"),
    ("GPU-Spine 可见性", "Runtime/Function/Framework/Component/Spine/SpineRenderer.cpp", "IsActiveInHierarchy"),
    ("点光源同步", "Runtime/Function/Render/RenderScene.cpp", "IsActiveInHierarchy"),
    ("主相机选取", "Runtime/Function/Framework/Level/Level.cpp", "IsActiveInHierarchy"),
]


def iter_source_files(root: Path = SOURCE_ROOT) -> list[Path]:
    """列出 Runtime + Editor 下的源文件。`root` 可换成临时副本（selftest 用）。"""
    files: list[Path] = []
    for base in (root / "Runtime", root / "Editor"):
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SCAN_SUFFIXES:
                files.append(path)
    return files


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */。

    注释里引旧代码（"这里以前是 transform->SetEnabled"）不该算违规 —— 这类注释正是
    这次改动留下的。不做字符串字面量解析：这种拼写出现在 C++ 字符串里概率为零，
    真出现了是误报而不是漏报（漏报才致命）。
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            # 块注释里的换行必须换成等量换行：直接删会让后面的行号整体前移，
            # 报错定位就错了（函数体范围判定也会跟着错）。
            out.append("\n" * text.count("\n", i, end))
            i = end
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end == -1 else end
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


SETACTIVE_SIGNATURE = re.compile(r"^\s*void\s+GameObject::SetActive\s*\(")


def setactive_body_lines(text: str) -> set[int]:
    """`GameObject::SetActive` 函数体覆盖的行号（1-based）。

    SetActive 自己当然要写 flag —— 它就是那个唯一入口。但不能因此放过整个
    GameObject.cpp：那样以后在别处新加的绕过会一起被放过。所以只认这一个函数体，
    用花括号配平确定范围（本函数体里没有字符串/字符字面量含花括号，够用）。
    """
    lines = text.splitlines()
    body: set[int] = set()
    depth = 0
    inside = False
    opened = False
    for index, line in enumerate(lines, start=1):
        if not inside:
            if SETACTIVE_SIGNATURE.match(line):
                inside = True
                depth = 0
            else:
                continue
        body.add(index)
        depth += line.count("{") - line.count("}")
        if depth > 0:
            opened = True
        elif opened and depth <= 0:
            # 花括号回到 0 且之前开过 —— 函数体结束。签名行本身没有 '{'，
            # 不能拿 "本行有 { 且 depth<=0" 当结束条件（那样永远不满足）。
            break
    return body


def display_path(path: Path) -> str:
    """仓库内显示相对路径；selftest 的临时副本不在仓库里，退回绝对路径。

    退回绝对路径不影响 selftest 的判定：临时目录里同样含 `Runtime/...` 这一段。
    """
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def check_direct_writes(files: list[Path]) -> list[str]:
    problems: list[str] = []
    for path in files:
        raw = path.read_text(encoding="utf-8", errors="replace")
        text = strip_comments(raw)
        allowed = setactive_body_lines(text) if path.name == "GameObject.cpp" else set()
        for line_no, line in enumerate(text.splitlines(), start=1):
            if DIRECT_TRANSFORM_SETENABLED.search(line) and line_no not in allowed:
                rel = display_path(path)
                problems.append(
                    f"{rel}:{line_no}: 直接写 Transform 的 enabled —— 应改走 "
                    f"GameObject::SetActive\n      {line.strip()}"
                )
    return problems


def check_consumers(root: Path = SOURCE_ROOT) -> list[str]:
    problems: list[str] = []
    for label, rel_path, needle in CONSUMERS:
        path = root / rel_path
        if not path.is_file():
            problems.append(f"{label}: 文件不存在 {rel_path}")
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        if needle not in text:
            problems.append(
                f"{label}: {rel_path} 里没有 {needle}() —— 停用后这一路不会跟着停"
            )
    return problems


def check_entry_point(root: Path = SOURCE_ROOT) -> list[str]:
    """SetActive 必须存在、且带脚本导出标记。"""
    header = root / "Runtime" / "BaseClasses" / "GameObject.h"
    problems: list[str] = []
    if not header.is_file():
        return [f"GameObject.h 不存在：{header}"]
    text = header.read_text(encoding="utf-8", errors="replace")
    if not re.search(r"ZFUNCTION\(Name\s*=\s*\"SetActive\"\)", text):
        problems.append("GameObject.h: SetActive 没有 ZFUNCTION(Name=\"SetActive\") 标记")
    if "ZSCRIPTPROPERTY(Name=\"activeSelf\"" not in text:
        problems.append("GameObject.h: 缺 activeSelf 访问器属性")
    if "ZSCRIPTPROPERTY(Name=\"activeInHierarchy\"" not in text:
        problems.append("GameObject.h: 缺 activeInHierarchy 访问器属性")
    return problems


# ---- 负向验证 -------------------------------------------------------------
# 注入下面这些（就是这次修掉的三处的原始写法），检查必须报错。不这么做的话，
# 一个正则写错 / 路径写错的检查会一直"通过"，比没有检查更糟。

INJECTIONS: list[tuple[str, str]] = [
    ("Runtime/Function/Timeline/TimelineDirector.cpp",
     "            transform->SetEnabled(activation_clip->m_Active);\n"),
    ("Runtime/Function/Framework/Component/Mesh/BaseRenderer.cpp",
     "    xf->SetEnabled(active);\n"),
    ("Runtime/BaseClasses/GameObject.cpp",
     "    t->SetEnabled(false);\n"),
]


def selftest() -> int:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        sandbox = Path(tmp) / "Source"
        shutil.copytree(SOURCE_ROOT, sandbox)

        for rel_path, snippet in INJECTIONS:
            target = sandbox / rel_path
            if not target.is_file():
                failures.append(f"selftest 注入目标不存在：{rel_path}")
                continue
            # 追加到文件末尾而不是插到中间：插中间得挑位置，容易插进注释里被
            # strip_comments 吃掉，那样 selftest 就假失败了。
            target.write_text(
                target.read_text(encoding="utf-8")
                + "\nvoid InjectedForSelftest()\n{\n" + snippet + "}\n",
                encoding="utf-8",
            )
            found = check_direct_writes(iter_source_files(sandbox))
            if not any(rel_path in p for p in found):
                failures.append(
                    f"selftest 失败：往 {rel_path} 注入 `{snippet.strip()}` 之后 "
                    f"check_direct_writes 仍然通过 —— 检查是摆设"
                )

        # 消费方检查同样要能被打破：把 GameObject.cpp 里的 IsActiveInHierarchy 全
        # 换掉，"Tick 门禁" 那一项必须报错。
        go = sandbox / "Runtime" / "BaseClasses" / "GameObject.cpp"
        go.write_text(
            go.read_text(encoding="utf-8").replace("IsActiveInHierarchy", "NotARealName"),
            encoding="utf-8",
        )
        consumer_problems = check_consumers(sandbox)
        if not any(p.startswith("Tick 门禁") for p in consumer_problems):
            failures.append(
                "selftest 失败：把 GameObject.cpp 的 IsActiveInHierarchy 改掉之后 "
                "check_consumers 仍然通过 —— 检查是摆设"
            )

        # 入口标记检查同理。
        hdr = sandbox / "Runtime" / "BaseClasses" / "GameObject.h"
        hdr.write_text(
            hdr.read_text(encoding="utf-8").replace(
                "ZFUNCTION(Name=\"SetActive\")", "/* marker removed for selftest */"
            ),
            encoding="utf-8",
        )
        if not check_entry_point(sandbox):
            failures.append(
                "selftest 失败：去掉 SetActive 的 ZFUNCTION 标记之后 "
                "check_entry_point 仍然通过 —— 检查是摆设"
            )

    if failures:
        print("SELFTEST FAIL")
        for f in failures:
            print("  " + f)
        return 1
    print(f"SELFTEST OK：注入 {len(INJECTIONS)} 处旧写法 + 破坏 2 处检查目标后，"
          f"检查确实都失败了。")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="SetActive 不变量自检")
    parser.add_argument(
        "--selftest", action="store_true", help="负向验证：确认检查不是摆设"
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    files = iter_source_files(SOURCE_ROOT)
    problems: list[str] = []
    problems += check_direct_writes(files)
    problems += check_consumers()
    problems += check_entry_point()

    print(f"扫描 {len(files)} 个源文件（Runtime + Editor）")

    if problems:
        print(f"\n{len(problems)} 项不合格：\n")
        for p in problems:
            print("  " + p)
        print("\n详见 doc/SCRIPT_SCENE_API.md §4。")
        return 1

    print("\n全部通过：")
    print("  · 没有绕过 GameObject::SetActive 直接写 Transform enabled 的地方")
    print(f"  · {len(CONSUMERS)} 个消费方都问 IsActiveInHierarchy()")
    print("  · SetActive / activeSelf / activeInHierarchy 都有脚本导出标记")
    return 0


if __name__ == "__main__":
    sys.exit(main())
