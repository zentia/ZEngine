"""Mutation harness for the `Transform` parity slice #45.

What is being protected here is *not* new maths -- `SetPositionAndRotation`,
`hasChanged`, `hierarchyCapacity` and `MoveAfterSibling` are all thin. The danger
is that each one is thin in a way that a wrong version still compiles, still runs,
and still returns something plausible:

* `SetPositionAndRotation` that silently forwards to the *local* version looks
  perfectly fine on a root object -- which is what 90% of test fixtures use. The
  fixture here has the parent at (10,0,0) specifically so world and local differ
  (TE3).
* `hasChanged` backed by the existing `Component::m_IsDirty` is the single most
  likely wrong implementation: the member already exists, already means "changed",
  and is already set everywhere. It fails only because the frame loop consumes it
  -- so the test has to pin "still false after I cleared it" *and* "IsDirty is a
  different thing" (TE17).
* `MoveAfterSibling`'s off-by-one fires only when the mover starts *before* the
  target, which is exactly the case nobody writes by hand (TEM01).
* `hierarchyCount` returning the subtree instead of the whole hierarchy is right
  for the root and wrong for everything else (TE18/TE20).

Two test gaps the first round found (both were real gaps, not bad mutants):

* TEM14 (`MoveAfterSibling` drops the `sibling == this` guard) survived: the only
  `MoveAfterSibling(self)` assertion ran while the mover was already **last**, and
  there the target index is clamped back to the end, so removing the guard is a
  no-op. Fixed with MA2b, which does it while self is first.
* TEM15 (the `MoveAfterSibling` ZFUNCTION is dropped) survived: the wiring gate
  grepped for the bare method name, which is still present in the declaration.
  Fixed by grepping `ZFUNCTION()` **plus the declaration line** -- same trap as
  `Behaviour`'s synthetic class: the string is there, the binding is not.

Known coverage gap (deliberately NOT written as a mutant):

* `Mover.ts` is not recompiled by this harness, so Z6/Z7 (the TypeScript probe)
  has no mutant of its own. It is a naming gate first -- `t.hasChanged` does not
  type-check if the ZSCRIPTPROPERTY is missing -- and its runtime half (matrix
  property vs `TransformPoint`) is covered by TE8/TE9/TE10 on the C++ side.

Run:  py .mutate_te.py            (all)
      py .mutate_te.py TEM01      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

TR_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Transform\Transform.h")
TR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Transform\Transform.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n    }\n"

# ⚠️ 这一片新加的 C++ 函数里，**目标行之前**就有 `if (...) { ... }`，那些 if 的
# 收尾花括号也是 4 空格缩进 —— 默认标记会停在那儿，把要改的行切到区域外面
# （第一轮 TEM08-TEM13 / TEM18 全是这么报 "PATTERN NOT FOUND" 的）。
# 这些函数都在全局作用域，所以真正的函数尾是**顶格**的那个 `}`。
END_OF_TOPLEVEL_FUNC = "\n}\n"

# ⚠️ 一次只编一个 target：同时给 `--target A B` 会让 MSBuild 并行跑，某个 target
# 的 post-build 往 bin/Debug/ZRuntime.lib 拷贝时正好撞上另一个的链接 —— 间歇性
# LNK1236。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    """Replace only inside one function body (see .mutate_tc.py for why)."""
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else END_OF_FUNC
    j = text.find(marker, i)
    if j < 0:
        raise SystemExit("END OF FUNC NOT FOUND: %r" % func_sig)
    region = text[i:j]
    if old not in region:
        raise SystemExit("PATTERN NOT FOUND in %s [%s]:\n%r" % (path, func_sig, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        text[:i] + region.replace(old, new, 1) + text[j:])


def run(exe):
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, errors="replace", timeout=300)
    out = r.stdout or ""
    fails = [l for l in out.splitlines() if "FAIL" in l]
    tail = [l for l in out.splitlines() if ("checks," in l or "项检查" in l)]
    print("  %-30s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:4]:
        print("      " + l.strip())
    return r.returncode


def build():
    last = None
    for _ in range(3):
        ok = True
        for target in TARGETS:
            p = subprocess.run(["cmake", "--build", ".", "--target", target,
                                "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                               env=ENV, capture_output=True, text=True)
            last = p
            if p.returncode != 0:
                ok = False
                break
        if ok:
            return last
    return last


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # ---- ⚠️ 头条：世界 / 局部 -------------------------------------------------
    # 转发到 local 版本。在**根对象**上完全看不出区别（根对象的世界坐标就是
    # 局部坐标），而绝大多数夹具都是根对象 —— 这就是非对称夹具（root 在
    # (10,0,0)）存在的理由。
    ("TEM01 SetPositionAndRotation forwards to the LOCAL version (root-only bug)",
     TR_CPP, "void Transform::SetPositionAndRotation(",
     "    SetPosition(world_position);\n    SetRotation(world_rotation);",
     "    SetLocalPosition(world_position);\n    SetLocalRotation(world_rotation);"),

    # 互补的一半：local 版本转发到世界版本。
    ("TEM02 SetLocalPositionAndRotation forwards to the WORLD version",
     TR_CPP, "void Transform::SetLocalPositionAndRotation(",
     "    SetLocalPosition(local_position);\n    SetLocalRotation(local_rotation);",
     "    SetPosition(local_position);\n    SetRotation(local_rotation);"),

    # 只设位置、漏了朝向：位置断言全绿，只有"朝向"那条（forward -Y -> +X）看得见。
    ("TEM03 SetPositionAndRotation drops the rotation",
     TR_CPP, "void Transform::SetPositionAndRotation(",
     "    SetPosition(world_position);\n    SetRotation(world_rotation);",
     "    SetPosition(world_position);"),

    # ---- hasChanged -----------------------------------------------------------
    # ⚠️ 最可能的错实现：Component 上现成就有一个 `m_IsDirty`，意思也差不多。
    # 它错在**帧循环会消费它** —— 轮询式用法（读完清零）会永远读不到 true。
    ("TEM04 hasChanged is Component::IsDirty() (the frame loop clears it)",
     TR_H, None,
     "    bool GetHasChanged() const { return m_HasChanged; }",
     "    bool GetHasChanged() const { return m_IsDirty; }"),

    # 置位漏在公共出口外面：写在 SetLocalPosition 里的话，SetParent /
    # SetSiblingIndex / MoveAfterSibling 就全都漏了。
    ("TEM05 MarkTransformDirty forgets to set hasChanged",
     TR_CPP, "void Transform::MarkTransformDirty(",
     "    m_HasChanged = true;",
     "    ;"),

    # 加载后不清零：脚本一拿到新 transform 就看到 true。
    ("TEM06 PostLoadResource does not clear hasChanged",
     TR_CPP, "void Transform::PostLoadResource(",
     "    m_HasChanged = false;",
     "    ;"),

    # setter 取反：写 false 得到 true。
    ("TEM07 SetHasChanged inverts its argument",
     TR_H, None,
     "    void SetHasChanged(bool has_changed) { m_HasChanged = has_changed; }",
     "    void SetHasChanged(bool has_changed) { m_HasChanged = !has_changed; }"),

    # ---- hierarchyCount / hierarchyCapacity -----------------------------------
    # 返回自己的子树：对根节点来说**是正确答案**，对其它节点全错。
    ("TEM08 GetHierarchyCount counts the subtree, not the whole hierarchy",
     TR_CPP, "int Transform::GetHierarchyCount() const",
     "        return static_cast<int>(GetDeepChildCount(*root->m_TransformData.hierarchy, 0));",
     "        return static_cast<int>(CountNodesDeep());", END_OF_TOPLEVEL_FUNC),

    # 用自己的下标查 deep_child_count：根节点（下标 0）读出来是对的。
    ("TEM09 GetHierarchyCount reads this node's slot instead of slot 0",
     TR_CPP, "int Transform::GetHierarchyCount() const",
     "        return static_cast<int>(GetDeepChildCount(*root->m_TransformData.hierarchy, 0));",
     "        return static_cast<int>(GetDeepChildCount(*m_TransformData.hierarchy, "
     "m_TransformData.index));", END_OF_TOPLEVEL_FUNC),

    # 扩容变成空操作：容量永远停在初始值，TE22 的 ">= 64" 抓它。
    ("TEM10 SetHierarchyCapacity silently does nothing",
     TR_CPP, "void Transform::SetHierarchyCapacity(",
     "    GrowTransformHierarchyCapacity(hierarchy, static_cast<uint32_t>(capacity));",
     "    (void)hierarchy;", END_OF_TOPLEVEL_FUNC),

    # 容量当成节点数报出来：TE22 同样抓它（这条与 TEM10 互补 —— 一个"没扩"、
    # 一个"扩了但读不出来"）。
    ("TEM11 GetHierarchyCapacity reports the count instead of the capacity",
     TR_CPP, "int Transform::GetHierarchyCapacity() const",
     "        return static_cast<int>(root->m_TransformData.hierarchy->transform_capacity);",
     "        return GetHierarchyCount();", END_OF_TOPLEVEL_FUNC),

    # ---- MoveAfterSibling ------------------------------------------------------
    # ⚠️ 头条 off-by-one：两个分支写反。"往前挪"（跨过目标）那一半变成空操作，
    # 而"往后挪"那一半会把人挪到目标**前面**。
    ("TEM12 MoveAfterSibling's two branches are swapped (the off-by-one)",
     TR_CPP, "void Transform::MoveAfterSibling(",
     "    const int target = (self_index < sibling_index) ? sibling_index : sibling_index + 1;",
     "    const int target = (self_index < sibling_index) ? sibling_index + 1 : sibling_index;", END_OF_TOPLEVEL_FUNC),

    # 统一 +1（不区分方向）：等价于"永远往后挪一位"，往前挪变成空操作。
    ("TEM13 MoveAfterSibling always uses sibling_index + 1",
     TR_CPP, "void Transform::MoveAfterSibling(",
     "    const int target = (self_index < sibling_index) ? sibling_index : sibling_index + 1;",
     "    const int target = sibling_index + 1;", END_OF_TOPLEVEL_FUNC),

    # 少了"是自己"这一道闸：A.MoveAfterSibling(A) 会把自己挪到 B 后面 —— 顺序
    # 真的变了，而且是"我什么都没做怎么顺序变了"这种最难查的表现。
    ("TEM14 MoveAfterSibling drops the 'sibling is this' guard",
     TR_CPP, "void Transform::MoveAfterSibling(",
     "    if (sibling == nullptr || sibling == this)",
     "    if (sibling == nullptr)", END_OF_TOPLEVEL_FUNC),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ 只有源码门看得见：C++ 照常工作，脚本里那个名字根本不存在 —— 而"方法
    # 不存在"在运行时是静默无事发生。
    ("TEM15 the MoveAfterSibling ZFUNCTION is dropped (C++ keeps working, the script name is gone)",
     TR_H, None,
     "    ZFUNCTION()\n    void MoveAfterSibling(Transform* sibling);",
     "    void MoveAfterSibling(Transform* sibling);"),

    # 属性拼写丢一条：脚本只能写 `t.GetHasChanged()`，与 Unity 不同形。
    ("TEM16 the hasChanged ZSCRIPTPROPERTY is dropped",
     TR_H, None,
     "    ZSCRIPTPROPERTY(Name=\"hasChanged\", Getter=\"GetHasChanged\", Setter=\"SetHasChanged\")",
     "    // (mutated away)"),

    # 矩阵属性丢一条。
    ("TEM17 the localToWorldMatrix ZSCRIPTPROPERTY is dropped",
     TR_H, None,
     "    ZSCRIPTPROPERTY(Name=\"localToWorldMatrix\", Getter=\"GetLocalToWorldMatrix\")",
     "    // (mutated away)"),

    # 接线门本身：MoveAfterSibling 必须复用修层级的那一段，而不是自己改一遍
    # m_Children（那样"UI 顺序变了、渲染顺序没变"）。
    ("TEM18 MoveAfterSibling reimplements the reorder instead of reusing ReorderWithinParent",
     TR_CPP, "void Transform::MoveAfterSibling(",
     "    ReorderWithinParent(parent, target);",
     "    (void)target; parent->RebuildTransformHierarchy();", END_OF_TOPLEVEL_FUNC),
]

LOCK = os.path.join(BAK, "zengine_mutate_te.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_te.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            end_marker = None
            if len(entry) == 6:
                name, path, func_sig, old, new, end_marker = entry
            elif len(entry) == 5:
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".tebak")
            shutil.copy2(path, bak)
            try:
                try:
                    if func_sig is not None:
                        mutate_in(path, func_sig, old, new, end_marker)
                    else:
                        mutate(path, old, new)
                except SystemExit as e:
                    print("  HARNESS ERROR: %s" % e)
                    results.append((name, "harness error (anchor not found)"))
                    continue
                verdict = "BUILD FAILED"
                for _ in range(3):
                    verdict = build_and_run()
                    if verdict != "BUILD FAILED":
                        break
                    print("  (retrying -- flaky MSBuild)")
                if verdict == "BUILD FAILED":
                    verdict = "invalid (does not build)"
                results.append((name, verdict))
                with io.open(os.path.join(ROOT, ".mutate_te.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        print("---- summary ----")
        for n, r in results:
            print("%-70s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
