"""Mutation harness for `OnTransformChildrenChanged` (the CH / CW / P1x slice).

Why this slice needs its own harness at all:

* The two callbacks are the SAME event seen from two sides, and their names
  differ by one word. Every "wrong side" mutant (TCM01 / TCM05 / TCM09) still
  compiles, still runs, and still leaves *a* callback firing -- just on the
  wrong object. Only an asymmetric fixture (parent has a probe AND the child
  has a probe) can see it, which is what the CH group is built around.
* CH29 asserts **0** ("reordering does not fire"). A gate that asserts 0 is
  vacuous by construction: an implementation that never fires at all also
  gives 0. CH29a/CH29b exist as the complementary half, and TCM07 is the
  mutant in the other direction (reordering DOES fire).
* The source-level gates (CW*) carry the weight that behaviour cannot: CW9
  counts the dispatch **twice** because the old-parent and new-parent blocks
  are textually identical, so `>= 1` would pass with either one deleted.

Run:  py .mutate_tc.py            (all)
      py .mutate_tc.py TCM03      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

COMPONENT_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Component.h")
GAMEOBJECT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
TRANSFORM_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Transform\Transform.cpp")
TS_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.cpp")
ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
PARITY_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Test\UnityParitySmokeTest.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n    }\n"

# ⚠️ 一次只编一个 target：同时给 `--target A B` 会让 MSBuild 并行跑，某个 target
# 的 post-build 往 bin/Debug/ZRuntime.lib 拷贝时正好撞上另一个的链接 —— 间歇性
# LNK1236。另外 LNK1236 本身就是偶发的：同一个 lib 重编一次就过，所以 build()
# 里失败会重试。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    # 允许成对替换（"把这一段搬到那里"需要删一处 + 加一处）。
    olds = old if isinstance(old, list) else [old]
    news = new if isinstance(new, list) else [new]
    for o, n in zip(olds, news):
        if o not in text:
            raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, o))
        text = text.replace(o, n, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


def mutate_in(path, func_sig, old, new):
    """Replace only inside one function body.

    锚点必须限定在函数内：`BroadcastAppEvent` 在 Transform.cpp 里不止一处，
    全局 replace 会改到别的入口上，得到一个"什么也没验证"的 SURVIVED。
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    j = text.find(END_OF_FUNC, i)
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
        for line in (p.stdout or "").splitlines()[-3:]:
            print("    " + line)
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


OLD_PARENT_BLOCK = (
    "    if (old_father != nullptr)\n"
    "    {\n"
    "        if (GameObject* const old_owner = old_father->GetParentObject())\n"
    "        {\n"
    "            old_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);\n"
    "        }\n"
    "    }\n")

NEW_PARENT_BLOCK = (
    "    if (new_parent != nullptr)\n"
    "    {\n"
    "        if (GameObject* const new_owner = new_parent->GetParentObject())\n"
    "        {\n"
    "            new_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);\n"
    "        }\n"
    "    }\n")

CHILDREN_CASE = (
    "            case AppEventKind::kChildrenChanged:\n"
    "                component->OnTransformChildrenChanged();\n")

MUTATIONS = [
    # ---- GameObject：加了 case 却调错函数 ----------------------------------
    # 五个 case 长得一模一样，调错一个照样编译、照样运行，只是那条消息永远没人收。
    ("TCM01 the new case calls OnTransformParentChanged", GAMEOBJECT_CPP,
     CHILDREN_CASE,
     "            case AppEventKind::kChildrenChanged:\n"
     "                component->OnTransformParentChanged();\n"),

    ("TCM02 the new case is gone entirely", GAMEOBJECT_CPP,
     CHILDREN_CASE + "                break;\n", ""),

    # ---- Transform：两个父级，两处派发 --------------------------------------
    # 只删一处：文本完全一样，`>= 1` 的那种门抓不到。
    ("TCM03 only the NEW parent is told (the old one never hears about losing)",
     TRANSFORM_CPP, OLD_PARENT_BLOCK, ""),

    ("TCM04 only the OLD parent is told (the new one never hears about gaining)",
     TRANSFORM_CPP, NEW_PARENT_BLOCK, ""),

    # 发给被搬的对象自己：这是"两个回调写反"里最像对的那个 —— 被搬的对象上
    # 确实已经有另一个回调了。
    ("TCM05 it is dispatched to the moved object instead of its new parent",
     TRANSFORM_CPP,
     "        if (GameObject* const new_owner = new_parent->GetParentObject())",
     "        if (GameObject* const new_owner = GetParentObject())"),

    # 判空不是防御性编程：脱离父级时 new_parent 就是 nullptr。
    ("TCM06 the new-parent null guard is gone (detaching dereferences nullptr)",
     TRANSFORM_CPP,
     "    if (new_parent != nullptr)\n"
     "    {\n"
     "        if (GameObject* const new_owner = new_parent->GetParentObject())",
     "    {\n"
     "        if (GameObject* const new_owner = new_parent->GetParentObject())"),

    # 整块搬到 SetParent 开头：层级还没更新就派发。回调照样被调一次（计数是绿的），
    # 但 Mover.ts 里读到的 `transform.childCount` 是**旧**值 —— P13 / P16 是判据。
    ("TCM07 the dispatch moves BEFORE the hierarchy is updated",
     TRANSFORM_CPP,
     [OLD_PARENT_BLOCK + NEW_PARENT_BLOCK, "    Transform* const old_root = GetRoot();"],
     ["", OLD_PARENT_BLOCK + NEW_PARENT_BLOCK + "    Transform* const old_root = GetRoot();"]),

    # 两个块的顺序反过来：行为上没人能察觉（都是"某处收到了一次"），只有 CH18
    # 那条顺序串能抓到 —— 这是 CH18 存在的唯一理由。
    ("TCM08 the old/new parents are notified in the other order",
     TRANSFORM_CPP,
     OLD_PARENT_BLOCK + NEW_PARENT_BLOCK,
     NEW_PARENT_BLOCK + OLD_PARENT_BLOCK),

    # 旧父级那条发成了另一个 kind。
    ("TCM09 the old-parent block broadcasts kParentChanged",
     TRANSFORM_CPP,
     "            old_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);",
     "            old_owner->BroadcastAppEvent(AppEventKind::kParentChanged, false);"),

    # ---- 重排：只认成员增减 --------------------------------------------------
    # 反向的那一半：CH29 断言 0，这个变异体让它变成 1。
    ("TCM10 SetSiblingIndex fires it after all", TRANSFORM_CPP,
     "    const int current_index = GetSiblingIndex();",
     "    if (GameObject* const reorder_owner = parent->GetParentObject())\n"
     "    {\n"
     "        reorder_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);\n"
     "    }\n"
     "    const int current_index = GetSiblingIndex();"),

    # ---- 又有人把 Tick 的判据加回来 ------------------------------------------
    # 探针默认启用，所以这两条只能靠"故意停用一次"的行为断言（CH31 / CH34）
    # 抓到 —— AW8 / AW9 扫的是 BroadcastAppEvent 的函数体，而 `isEnabled`
    # 恰好不在它们的字符串清单里。这里证明行为门那一层不是摆设。
    ("TCM11 the children case gates on isEnabled", GAMEOBJECT_CPP,
     CHILDREN_CASE,
     "            case AppEventKind::kChildrenChanged:\n"
     "                if (component->isEnabled())\n"
     "                {\n"
     "                    component->OnTransformChildrenChanged();\n"
     "                }\n"),

    ("TCM12 the children case gates on activeInHierarchy", GAMEOBJECT_CPP,
     CHILDREN_CASE,
     "            case AppEventKind::kChildrenChanged:\n"
     "                if (component->GetParentObject() != nullptr &&\n"
     "                    component->GetParentObject()->IsActiveInHierarchy())\n"
     "                {\n"
     "                    component->OnTransformChildrenChanged();\n"
     "                }\n"),

    # ---- 脚本侧转发 ----------------------------------------------------------
    # 名字只差一个词。写错的人两个回调都在，只是该来的那个永远不来。
    ("TCM13 the script component forwards the other half's name", TS_CPP,
     '    InvokeMethod("OnTransformChildrenChanged");',
     '    InvokeMethod("OnTransformParentChanged");'),

    ("TCM14 the script component does not forward it at all", TS_CPP,
     '    InvokeMethod("OnTransformChildrenChanged");', ""),

    # ---- 发布面 ---------------------------------------------------------------
    ("TCM15 the JS prototype has no no-op default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnTransformChildrenChanged = function() {};"\n',
     ""),

    ("TCM16 the .d.ts declaration is gone", BINDING_CPP,
     '        "    OnTransformChildrenChanged(): void;",\n', ""),

    # 只改空白：声明语义还在、还能编译、tsc 也照样过，但 CW20 数的是字面量。
    ("TCM17 the .d.ts declaration is spelled differently", BINDING_CPP,
     '        "    OnTransformChildrenChanged(): void;",',
     '        "    OnTransformChildrenChanged() : void;",'),

    # ---- 门本身的门：读源文件时**不许**把 CRLF 原样带进来 --------------------
    #
    # 这一条是实测踩出来的，不是推演的：`Transform.cpp` 被 `git checkout` 翻成
    # CRLF 之后，`find("\n}\n")` 定位函数尾一律落空，`substr` 退化成"半个文件"
    # —— 于是 CW11（正向）变**永真**、CW15（反向）变**永假**。两种都是静默的，
    # 而且只在"这个文件恰好是 CRLF"时才出现。
    #
    # CW21/CW22 数的是 ReadFile 的返回语句，不依赖任何文件的行尾，所以拿掉
    # NormalizeNewlines 这一步它必定红；CW10b / CW14b 则证明"切出来的真的是
    # 一个函数"。
    ("TCM18 the reader stops normalizing CRLF before slicing function bodies",
     PARITY_CPP,
     "        return NormalizeNewlines(ss.str());",
     "        return ss.str();"),
]

LOCK = os.path.join(BAK, "zengine_mutate_tc.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_tc.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            if len(entry) == 5:
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            # 备份**工作副本**（不是 HEAD）：这次的改动还没提交，从 git show 取
            # 会把切片整个回滚掉。
            bak = os.path.join(BAK, os.path.basename(path) + ".tcbak")
            shutil.copy2(path, bak)
            try:
                try:
                    if func_sig is not None:
                        mutate_in(path, func_sig, old, new)
                    else:
                        mutate(path, old, new)
                except SystemExit as e:
                    # 锚点对不上（源码被别人改了 / 函数体切错了）不该掀桌子：
                    # 记下来继续跑下一个，否则前面十几个的结果全白跑。
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
                with io.open(os.path.join(ROOT, ".mutate_tc.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-64s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        print("---- summary ----")
        for n, r in results:
            print("%-64s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
