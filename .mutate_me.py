"""Mutation harness for the seven Unity mouse messages (the ME / MW / S slice).

Why this slice needs its own harness:

* The headline behaviour is a NEGATIVE again: "a disabled component STILL gets
  OnMouse*". MEM30 is the well-meaning "fix" (add an enabled gate) and ME27 is
  the runtime half -- complemented by ME28, which demands the very same probe
  be SKIPPED by Tick in that state. Without ME28, ME27 is vacuous.
* Drag / Up / UpAsButton go to the PRESSED object, not the hovered one. MEM04 /
  MEM05 swap them; only ME19 / ME22 (mouse moved away while held) can see it --
  a "click without moving" test would stay green.
* `OnMouseUpAsButton` differs from `OnMouseUp` by exactly one predicate. MEM06
  (always fire) and MEM07 (never fire) are the two directions.
* `~GameObject` calling `MouseEventDispatcher::Forget` is the one place in this
  slice where a missing line is a real use-after-free rather than a missing
  feature. MEM12 is that mutant.
* `OnMouseUp` is a PREFIX of `OnMouseUpAsButton`. Every source-level gate in
  this area therefore counts a string that includes the closing quote / the
  ` = function` / the `(` -- MEM51..MEM56 exist to prove those gates actually
  fire rather than being satisfied by the longer name.

Known coverage gap: `MouseEventDispatcher::PickAt`'s HIT path (a real camera +
a real collider) has no behavioural gate -- headless has no main camera, so
`TryPickRay` always returns false. `DispatchTo(state, under_mouse)` is fully
covered because the test feeds `under_mouse` directly. Closing that gap needs a
fixture that can install a main camera and a collider; that belongs to the
camera/physics work, not this slice.

Run:  py .mutate_me.py            (all)
      py .mutate_me.py MEM04      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

COMPONENT_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Component.h")
GAMEOBJECT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
DISPATCHER_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\MouseEventDispatcher.cpp")
APPLICATION_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Application\Application.cpp")
TS_H = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.h")
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
# LNK1236（md5 两份相同照样报）。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    olds = old if isinstance(old, list) else [old]
    news = new if isinstance(new, list) else [new]
    for o, n in zip(olds, news):
        if o not in text:
            raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, o))
        text = text.replace(o, n, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


def mutate_in(path, func_sig, old, new, end_marker=None):
    """Replace only inside one function body.

    ⚠️ `END_OF_FUNC` ("\\n    }\\n") 只能定位**第一个**缩进 4 的收尾花括号。
    `BroadcastMouseEvent`（里面有 `if (!IsActiveInHierarchy()) { return; }`）和
    `DispatchTo`（里面有 `if (under_mouse != s_Hovered) { ... }`）都会在更早的
    位置命中，于是默认锚点会把切片截在函数体**中间** —— 表现为 "PATTERN NOT
    FOUND"，看起来像源码变了，其实只是切片切短了。两者都必须显式给
    `end_marker`（下一个顶层定义的开头）。
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else END_OF_FUNC
    j = text.find(marker, i)
    if j < 0:
        raise SystemExit("END OF FUNC NOT FOUND: %r (marker %r)" % (func_sig, marker))
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
                                "--config", "Debug", "--parallel", "1"],
                               cwd=os.path.join(ROOT, "build"),
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


# ---- 锚点 ------------------------------------------------------------------
# `BroadcastMouseEvent` 的下一个顶层定义。
GO_END = "\nbool GameObject::HasComponent"
# `DispatchTo` 的下一个顶层定义。
DISPATCH_END = "\nvoid MouseEventDispatcher::Forget"
FORGET_END = "\nvoid MouseEventDispatcher::Reset"
RESET_END = "\nGameObject* MouseEventDispatcher::GetHovered"

EXIT_BLOCK = (
    "        if (s_Hovered != nullptr)\n"
    "        {\n"
    "            s_Hovered->BroadcastMouseEvent(MouseEventKind::kExit);\n"
    "        }\n")

ACTIVE_GATE = (
    "    if (!IsActiveInHierarchy())\n"
    "    {\n"
    "        return;\n"
    "    }\n")

# `BroadcastAppEvent` / `BroadcastDrawGizmos` 里也有一份一模一样的循环头，所以
# 这段必须配 func_sig 用（mutate_in 把范围限定在函数体内）。
LOOP_HEADER = (
    "    for (auto& component : m_Components)\n"
    "    {\n"
    "        if (component == nullptr)\n"
    "        {\n"
    "            continue;\n"
    "        }\n")

DRAG_CASE = (
    "            case MouseEventKind::kDrag:\n"
    "                component->OnMouseDrag();\n"
    "                break;\n")

UP_CASE = (
    "            case MouseEventKind::kUp:\n"
    "                component->OnMouseUp();\n"
    "                break;\n")

UPASBUTTON_GATE = (
    "        if (s_Pressed == s_Hovered)\n"
    "        {\n"
    "            s_Pressed->BroadcastMouseEvent(MouseEventKind::kUpAsButton);\n"
    "        }\n")

# Application.cpp：鼠标帧整块。
MOUSE_BLOCK = (
    "    {\n"
    "        MouseFrameState mouse_state;\n"
    "        mouse_state.position_pixels = Input::GetMousePosition();\n"
    "        mouse_state.pressed = Input::GetMouseButtonDown(0);\n"
    "        mouse_state.held = Input::GetMouseButton(0);\n"
    "        mouse_state.released = Input::GetMouseButtonUp(0);\n"
    "        MouseEventDispatcher::Tick(mouse_state);\n"
    "    }\n")

WORLD_TICK = "    GET_SYSTEM(WorldManager)->Tick(delta_time);\n"

MUTATIONS = [
    # ---- 派发器：进出 --------------------------------------------------------
    # ⚠️ 最容易犯也最难发现的一个：Exit 发给**新**对象。表现是鼠标从 A 移到 B 时
    # A 永远收不到 Exit（"高亮不消失"），而没有任何报错。
    ("MEM01 OnMouseExit goes to the NEW object instead of the old one", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     EXIT_BLOCK,
     "        if (under_mouse != nullptr)\n"
     "        {\n"
     "            under_mouse->BroadcastMouseEvent(MouseEventKind::kExit);\n"
     "        }\n",
     DISPATCH_END),

    ("MEM02 OnMouseEnter is never dispatched", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     "            under_mouse->BroadcastMouseEvent(MouseEventKind::kEnter);\n", "",
     DISPATCH_END),

    ("MEM03 OnMouseOver is never dispatched (only the edges fire)", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     "        s_Hovered->BroadcastMouseEvent(MouseEventKind::kOver);\n", "",
     DISPATCH_END),

    # 悬停状态不更新：Enter 于是每帧都来（Over 退化成 Enter），Exit 永不来。
    ("MEM04 the hovered object is never updated (Enter fires every frame)", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     "        s_Hovered = under_mouse;\n", "", DISPATCH_END),

    # ---- 派发器：Drag / Up 发给**按下时**那个 --------------------------------
    ("MEM05 OnMouseDrag follows the HOVERED object instead of the pressed one",
     DISPATCHER_CPP, "MouseEventDispatcher::DispatchTo",
     "        s_Pressed->BroadcastMouseEvent(MouseEventKind::kDrag);",
     "        s_Hovered->BroadcastMouseEvent(MouseEventKind::kDrag);",
     DISPATCH_END),

    ("MEM06 OnMouseUp goes to the HOVERED object instead of the pressed one",
     DISPATCHER_CPP, "MouseEventDispatcher::DispatchTo",
     "        s_Pressed->BroadcastMouseEvent(MouseEventKind::kUp);",
     "        s_Hovered->BroadcastMouseEvent(MouseEventKind::kUp);",
     DISPATCH_END),

    # ⚠️ 按钮语义：少了这个判据，"按下后拖出去再松手"也算点击。
    ("MEM07 OnMouseUpAsButton fires even when the release is elsewhere", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     UPASBUTTON_GATE,
     "        s_Pressed->BroadcastMouseEvent(MouseEventKind::kUpAsButton);\n",
     DISPATCH_END),

    ("MEM08 OnMouseUpAsButton never fires", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     "            s_Pressed->BroadcastMouseEvent(MouseEventKind::kUpAsButton);\n", "",
     DISPATCH_END),

    # 松手之后不摘：下一次 Drag 会打到上一次按下的对象上。
    ("MEM09 the press is never forgotten on release", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     "            s_Pressed->BroadcastMouseEvent(MouseEventKind::kUpAsButton);\n"
     "        }\n"
     "        s_Pressed = nullptr;\n",
     "            s_Pressed->BroadcastMouseEvent(MouseEventKind::kUpAsButton);\n"
     "        }\n",
     DISPATCH_END),

    # 按下时没记住对象：后面 Drag / Up 全都无处可发。
    ("MEM10 the press is never recorded", DISPATCHER_CPP,
     "MouseEventDispatcher::DispatchTo",
     "        s_Pressed = s_Hovered;\n", "", DISPATCH_END),

    # ---- Forget / Reset：本切片唯一一处真的会崩的地方 ------------------------
    ("MEM11 Forget clears the hover but not the press", DISPATCHER_CPP,
     "void MouseEventDispatcher::Forget",
     "    if (s_Pressed == object)\n"
     "    {\n"
     "        s_Pressed = nullptr;\n"
     "    }\n", "",
     FORGET_END),

    ("MEM12 Forget is a no-op (destroying the hovered object leaves a dangling pointer)",
     DISPATCHER_CPP, "void MouseEventDispatcher::Forget",
     "    if (s_Hovered == object)\n"
     "    {\n"
     "        s_Hovered = nullptr;\n"
     "    }\n", "",
     FORGET_END),

    # ⚠️ 只清 hovered 不清 pressed 是最容易漏的一种：切关卡时"恰好按着鼠标"才
    # 出现，概率低但确实是 use-after-free。ME44 是那条判据。
    ("MEM13 Reset clears the hover but not the press", DISPATCHER_CPP,
     "void MouseEventDispatcher::Reset",
     "    s_Pressed = nullptr;\n", "", RESET_END),

    ("MEM14 Reset is a no-op", DISPATCHER_CPP, "void MouseEventDispatcher::Reset",
     "    s_Hovered = nullptr;\n", "", RESET_END),

    # ---- GameObject::BroadcastMouseEvent 的判据 ------------------------------
    #
    # ⚠️ 头条：看起来像修 bug 的那一个。Unity 明文说 enabled 只挡 Start / Awake /
    # Update / FixedUpdate / OnGUI，鼠标消息不在那份清单里。
    ("MEM15 someone 'fixes' it by also gating on enabled", GAMEOBJECT_CPP,
     "void GameObject::BroadcastMouseEvent",
     LOOP_HEADER,
     LOOP_HEADER +
     "        if (!component->isEnabled())\n"
     "        {\n"
     "            continue;\n"
     "        }\n",
     GO_END),

    ("MEM16 the activeInHierarchy gate is gone", GAMEOBJECT_CPP,
     "void GameObject::BroadcastMouseEvent", ACTIVE_GATE, "", GO_END),

    ("MEM17 only the first component on an object gets mouse events", GAMEOBJECT_CPP,
     "void GameObject::BroadcastMouseEvent",
     LOOP_HEADER,
     "    bool mouse_first = true;\n" + LOOP_HEADER +
     "        if (!mouse_first) { continue; }\n"
     "        mouse_first = false;\n",
     GO_END),

    # 七个 case 长得一模一样：少一个 case 就是那条消息永远收不到。
    ("MEM18 the kDrag case is missing", GAMEOBJECT_CPP,
     "void GameObject::BroadcastMouseEvent", DRAG_CASE, "", GO_END),

    # 复制粘贴错的那一类：kUp 调了 UpAsButton。
    ("MEM19 kUp calls OnMouseUpAsButton (copy-paste slip)", GAMEOBJECT_CPP,
     "void GameObject::BroadcastMouseEvent",
     UP_CASE,
     "            case MouseEventKind::kUp:\n"
     "                component->OnMouseUpAsButton();\n"
     "                break;\n",
     GO_END),

    # ⚠️ 写在头文件里没人调 = 没有。这一条不修就真的崩。
    ("MEM20 the destructor no longer forgets itself", GAMEOBJECT_CPP,
     "    MouseEventDispatcher::Forget(this);\n", ""),

    # ---- 主循环 --------------------------------------------------------------
    ("MEM21 the logical tick no longer drives the mouse frame", APPLICATION_CPP,
     "        MouseEventDispatcher::Tick(mouse_state);\n", ""),

    ("MEM22 the held state is never filled in (OnMouseDrag never fires)", APPLICATION_CPP,
     "        mouse_state.held = Input::GetMouseButton(0);\n", ""),

    ("MEM23 the pressed edge is never filled in (OnMouseDown never fires)", APPLICATION_CPP,
     "        mouse_state.pressed = Input::GetMouseButtonDown(0);\n", ""),

    ("MEM24 the released edge is never filled in (OnMouseUp never fires)", APPLICATION_CPP,
     "        mouse_state.released = Input::GetMouseButtonUp(0);\n", ""),

    # 相位：挪到世界 tick 之后，脚本的 OnUpdate 要等到下一帧才看得到这一下点击。
    #
    # ⚠️ 替换的是 `WORLD_TICK + MOUSE_BLOCK`（块在**后面**）。写成
    # `MOUSE_BLOCK + WORLD_TICK` 的话块被搬到"世界 tick 之前"，MW33 那条
    # `at_input < at_mouse < at_world` 依旧成立 —— 变异体就白写了（第一次跑
    # 就是这个版本，NOT CAUGHT）。
    ("MEM25 the mouse frame runs AFTER the world tick", APPLICATION_CPP,
     [MOUSE_BLOCK, WORLD_TICK],
     ["", WORLD_TICK + MOUSE_BLOCK]),

    # ---- 组件面 --------------------------------------------------------------
    #
    # ⚠️ 单删 Component.h 里那一行编译不过（TypeScriptComponent 还 override 着），
    # 所以这里做的是"引擎里把钩子改名、脚本面还是旧名字" —— 这正是 MW1_4 那
    # 条门存在的理由：改名之后 `virtual void OnMouseDrag(` 在 Component.h 里
    # 变成 0 次，门必须响。
    ("MEM26 the OnMouseDrag hook is renamed in the engine but the script face keeps "
     "the old name",
     [(COMPONENT_H, "    virtual void OnMouseDrag() {}", "    virtual void OnMouseDragg() {}"),
      (GAMEOBJECT_CPP, "component->OnMouseDrag();", "component->OnMouseDragg();"),
      (TS_H, "void OnMouseDrag() override;", "void OnMouseDragg() override;"),
      (TS_CPP, "void TypeScriptComponent::OnMouseDrag()",
       "void TypeScriptComponent::OnMouseDragg()"),
      (PARITY_CPP, "void OnMouseDrag() override", "void OnMouseDragg() override")]),

    # ---- 脚本侧转发 ----------------------------------------------------------
    # 名字只差一两个词：写成 OnMouseDown 的话"按住不动的每一帧"都变成一次新的按下。
    ("MEM27 the script component forwards Drag as Down", TS_CPP,
     '    InvokeMethod("OnMouseDrag");', '    InvokeMethod("OnMouseDown");'),

    ("MEM28 the script component does not forward OnMouseUp at all", TS_CPP,
     '    InvokeMethod("OnMouseUp");\n', ""),

    ("MEM29 the script component does not forward OnMouseUpAsButton", TS_CPP,
     '    InvokeMethod("OnMouseUpAsButton");\n', ""),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ `OnMouseUp` 是 `OnMouseUpAsButton` 的前缀：门必须带 " = function"。
    ("MEM30 the JS prototype has no OnMouseUp default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnMouseUp = function() {};"\n', ""),

    ("MEM31 the JS prototype has no OnMouseUpAsButton default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnMouseUpAsButton = function() {};"\n', ""),

    ("MEM32 the .d.ts declaration of OnMouseDown is gone", BINDING_CPP,
     '        "    OnMouseDown(): void;",\n', ""),

    ("MEM33 the .d.ts declaration of OnMouseUpAsButton is gone", BINDING_CPP,
     '        "    OnMouseUpAsButton(): void;",\n', ""),

    ("MEM34 the .d.ts declares OnMouseUpAsButton under a different spelling", BINDING_CPP,
     '        "    OnMouseUpAsButton(): void;",',
     '        "    OnMouseUpAsBtn(): void;",'),

    # ---- 门本身的门 ----------------------------------------------------------
    # MW6 / MW7 是"函数体切片"上的门：不归一 CRLF 的话切片退化成半行，门变永假
    # （看起来像"有人把判据加回来了"，其实只是行尾变了）。
    ("MEM35 the reader stops normalizing CRLF before slicing function bodies", PARITY_CPP,
     "        return NormalizeNewlines(ss.str());",
     "        return ss.str();"),

    # ⚠️ 互补的一半：把"不判 enabled"退化成"enabled 这个开关压根没接上"。
    # 没有它，ME27 那条"停用照样收"的断言是空的（一次都没被挡过，"照样收"就
    # 说明不了任何事）。
    ("MEM36 the probe's enabled flag is ignored everywhere (ME27 becomes vacuous)",
     PARITY_CPP,
     "        bool isEnabled() const override { return m_ProbeEnabled; }",
     "        bool isEnabled() const override { return true; }"),
]

LOCK = os.path.join(BAK, "zengine_mutate_me.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_me.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            if len(entry) == 2 and isinstance(entry[1], list):
                # 多文件变异体（改名那一类：单删一个文件编译不过）。
                name, steps = entry
                paths = [s[0] for s in steps]
            else:
                steps = None
                end_marker = None
                if len(entry) == 6:
                    name, path, func_sig, old, new, end_marker = entry
                elif len(entry) == 5:
                    name, path, func_sig, old, new = entry
                else:
                    name, path, old, new = entry
                    func_sig = None
                paths = [path]
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            # 备份**工作副本**（不是 HEAD）：这次的改动还没提交。
            baks = []
            for p in paths:
                bak = os.path.join(BAK, os.path.basename(p) + ".mebak")
                shutil.copy2(p, bak)
                baks.append((p, bak))
            try:
                try:
                    if steps is not None:
                        for p, o, n in steps:
                            mutate(p, o, n)
                    elif func_sig is not None:
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
                with io.open(os.path.join(ROOT, ".mutate_me.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-64s %s\n" % (name, verdict))
            finally:
                for p, bak in baks:
                    shutil.copy2(bak, p)
                    os.utime(p, None)
                    touched.add(p)
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
