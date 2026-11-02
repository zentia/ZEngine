"""Mutation harness for `OnDrawGizmos` / `OnDrawGizmosSelected` (the DG / DW / R slice).

Why this slice needs its own harness at all:

* The headline behaviour is a NEGATIVE: "a disabled component STILL draws
  gizmos". A gate that asserts a negative is vacuous by construction -- an
  implementation where `enabled` was never wired to anything also passes.
  DGM03 is that complementary mutant (the probe's `enabled` flag is read but
  nothing consults it), and DG23/DG24 in the test are the runtime half:
  the same probe must be SKIPPED by Tick in the very same state.
* DGM01 ("also gate on enabled") is the mutant most likely to be introduced
  by a well-meaning future reader: it looks like a bug fix. DW6 is the
  source-level reverse gate for exactly that, and DGM01 is what proves DW6
  actually fires.
* Selection is ADDITIVE, not either/or. DGM05 (only Selected when selected)
  and DGM06 (only ever Selected) are the two directions; both still leave a
  callback firing, so only the pair of counters can see them.
* Level::DrawGizmos walks TWO tables with textually identical call sites, so
  `>= 1` style gates pass with either one deleted. DGM09/DGM10 delete one each;
  DW14 (which counts 2) is what must catch them.

Run:  py .mutate_dg.py            (all)
      py .mutate_dg.py DGM03      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

COMPONENT_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Component.h")
GAMEOBJECT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
LEVEL_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Level\Level.cpp")
WORLD_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\World\WorldManager.cpp")
APPLICATION_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Application\Application.cpp")
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
    """Replace only inside one function body (see .mutate_tc.py for why).

    ⚠️ `END_OF_FUNC` (`"\\n    }\\n"`) 只能定位**第一个**缩进 4 的收尾花括号。
    `BroadcastDrawGizmos` / `Level::DrawGizmos` 里都有更早的 `if (...) { return; }`，
    于是默认锚点会把切片截在函数体**中间**（"半个函数"）—— 表现是
    "PATTERN NOT FOUND"，看起来像源码变了，其实只是切片切短了。
    所以这两个函数必须显式给 `end_marker`（下一个顶层定义的开头）。
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


# `GameObject::BroadcastDrawGizmos` 里那两条派发。注意注释里**没有**
# "isEnabled" 这个词（DW6 是反向门），所以改动注释不会影响判定。
DRAW_BODY = (
    "        component->OnDrawGizmos();\n"
    "        if (selected)\n"
    "        {\n"
    "            component->OnDrawGizmosSelected();\n"
    "        }\n")

ACTIVE_GATE = (
    "    if (!IsActiveInHierarchy())\n"
    "    {\n"
    "        return;\n"
    "    }\n")

GO_END = "\n// Both lookups go through"

# `Application.cpp`：`DrawGizmosFrame` 的下一个顶层定义。
APP_RESOLVE = "\nstd::vector<GameObject*> Application::ResolveGizmoSelection"

# `BroadcastAppEvent` 里也有一份一模一样的循环头，所以这段必须配 func_sig 用
# （mutate_in 会把范围限定在函数体内）。
# ⚠️ 结尾**不带**空行：`continue; }` 与 `component->OnDrawGizmos();` 之间还夹着
# 一段注释，把它算进来的话锚点对不上（DGM08 第一次就是这么死的）。
LOOP_HEADER = (
    "    for (auto& component : m_Components)\n"
    "    {\n"
    "        if (component == nullptr)\n"
    "        {\n"
    "            continue;\n"
    "        }\n")
LEVEL_END = "\nvoid Level::LateTick"

RUNTIME_TABLE_BLOCK = (
    "    for (const auto& id_object_pair : m_RuntimeGObjects)\n"
    "    {\n"
    "        if (id_object_pair.second != nullptr)\n"
    "        {\n"
    "            id_object_pair.second->BroadcastDrawGizmos("
    "is_selected(id_object_pair.second));\n"
    "        }\n"
    "    }\n")

MAIN_TABLE_BLOCK = (
    "    for (const auto& id_object_pair : m_Gobjects)\n"
    "    {\n"
    "        if (id_object_pair.second)\n"
    "        {\n"
    "            id_object_pair.second->BroadcastDrawGizmos("
    "is_selected(id_object_pair.second.get()));\n"
    "        }\n"
    "    }\n")

MUTATIONS = [
    # ---- ⚠️ 头条："顺手修掉"那一类 ------------------------------------------
    # Unity 明文说 enabled 只挡 Start/Awake/Update/FixedUpdate/OnGUI，所以这里
    # **不该**判 enabled。这个变异体看起来像修 bug，是最可能真的被写进去的一个。
    # DG22（运行时）与 DW6（源码反向门）都该红 —— 两道门一起验。
    ("DGM01 someone 'fixes' it by also gating on enabled", GAMEOBJECT_CPP,
     "void GameObject::BroadcastDrawGizmos",
     DRAW_BODY,
     "        if (!component->isEnabled())\n"
     "        {\n"
     "            continue;\n"
     "        }\n" + DRAW_BODY,
     GO_END),

    # 正向判据被整个删掉：停用的对象也画。
    ("DGM02 the activeInHierarchy gate is gone", GAMEOBJECT_CPP,
     "void GameObject::BroadcastDrawGizmos", ACTIVE_GATE, "", GO_END),

    # ⚠️ 互补的一半：把"不判 enabled"退化成"enabled 这个开关压根没接上"。
    # 只有 DG23/DG24（同一状态下 Tick 必须被跳过 / 必须能回来）能看出来。
    ("DGM03 the probe's enabled flag is ignored everywhere (DG22 becomes vacuous)",
     PARITY_CPP,
     "        bool isEnabled() const override { return m_ProbeEnabled; }",
     "        bool isEnabled() const override { return true; }"),

    # ---- 选中语义 ------------------------------------------------------------
    # Selected 变成二选一：选中对象就不画常驻那条了。
    ("DGM04 selected objects get ONLY OnDrawGizmosSelected (either/or, not additive)",
     GAMEOBJECT_CPP, "void GameObject::BroadcastDrawGizmos",
     DRAW_BODY,
     "        if (selected)\n"
     "        {\n"
     "            component->OnDrawGizmosSelected();\n"
     "        }\n"
     "        else\n"
     "        {\n"
     "            component->OnDrawGizmos();\n"
     "        }\n",
     GO_END),

    # Selected 永不派发。
    ("DGM05 OnDrawGizmosSelected is never dispatched", GAMEOBJECT_CPP,
     "void GameObject::BroadcastDrawGizmos",
     "            component->OnDrawGizmosSelected();\n", "", GO_END),

    # 反过来：不看 selected，永远都发 Selected。
    ("DGM06 OnDrawGizmosSelected fires for every object", GAMEOBJECT_CPP,
     "void GameObject::BroadcastDrawGizmos",
     "        if (selected)\n", "        if (true)\n", GO_END),

    # 两个钩子的顺序反过来：只有 DG15/DG16 那条相邻子串能抓到。
    ("DGM07 the two hooks fire in the other order", GAMEOBJECT_CPP,
     "void GameObject::BroadcastDrawGizmos",
     DRAW_BODY,
     "        if (selected)\n"
     "        {\n"
     "            component->OnDrawGizmosSelected();\n"
     "        }\n"
     "        component->OnDrawGizmos();\n",
     GO_END),

    # 只发给第一个组件：DG6（同一个对象上挂两个探针）是唯一能看见的地方。
    ("DGM08 only the first component on an object gets OnDrawGizmos", GAMEOBJECT_CPP,
     "void GameObject::BroadcastDrawGizmos",
     LOOP_HEADER,
     "    bool gizmo_first = true;\n" + LOOP_HEADER +
     "        if (!gizmo_first) { continue; }\n"
     "        gizmo_first = false;\n",
     GO_END),

    # ---- Level：两张表，两处文本完全一样的调用 ------------------------------
    ("DGM09 the runtime (Instantiate) table is not walked", LEVEL_CPP,
     "void Level::DrawGizmos", RUNTIME_TABLE_BLOCK, "", LEVEL_END),

    ("DGM10 the loaded object table is not walked", LEVEL_CPP,
     "void Level::DrawGizmos", MAIN_TABLE_BLOCK, "", LEVEL_END),

    # 未加载的关卡：m_Gobjects 里是上一份的残留。
    # ⚠️ 这个变异体必须**连同函数签名**一起给：`if (!m_IsLoaded) { return; }` 在
    # Level.cpp 里出现很多次，全局 replace 会改到 Tick / LateTick 头上，得到一个
    # "什么也没验证"的结果。
    ("DGM11 an unloaded level still dispatches", LEVEL_CPP,
     "void Level::DrawGizmos(const std::vector<GameObject*>& selected_objects)\n"
     "{\n"
     "    if (!m_IsLoaded)\n"
     "    {\n"
     "        return;\n"
     "    }\n",
     "void Level::DrawGizmos(const std::vector<GameObject*>& selected_objects)\n"
     "{\n"),

    # 选中判定退化成"全都算选中"。
    ("DGM12 everything counts as selected", LEVEL_CPP,
     "void Level::DrawGizmos",
     "            if (selected == object)\n", "            if (true)\n", LEVEL_END),

    # ---- 路由 ----------------------------------------------------------------
    #
    # ⚠️ DGM13 被**删除**了，不是忘了写。原意是"WorldManager 那层转发被改坏"，
    # 但 headless 下 `WorldManager::DrawGizmos` 的两个分支都走不到：
    # `m_CurrentActiveLevel` 只能由真实加载流程设置（`m_LoadedLevels` 是私有的、
    # 也没有 setter），所以"有活动关卡"这一支没有夹具；"没有活动关卡"这一支
    # 在有没有那段代码时行为完全一致。留一个必然 NOT CAUGHT 的变异体等于自欺，
    # 所以这里改成把缺口写清楚：
    #
    #   **已知覆盖缺口**：`WorldManager::DrawGizmos` 只有源码级门（DW15），没有
    #   行为级门。`Level::DrawGizmos` 往下（两张表、判据、脚本转发）是全覆盖的。
    #
    # 想补的话需要给 WorldManager 一个"把这张 Level 设为活动关卡"的测试接缝，
    # 那会为测试往生产 API 里加东西 —— 不划算，先记着。

    # ---- 脚本侧转发 ----------------------------------------------------------
    # 名字只差一个词：写错的话选中对象的常驻辅助线照来，仅选中的那次永远不来。
    ("DGM14 the script component forwards the wrong hook name", TS_CPP,
     '    InvokeMethod("OnDrawGizmosSelected");',
     '    InvokeMethod("OnDrawGizmos");'),

    ("DGM15 the script component does not forward OnDrawGizmos at all", TS_CPP,
     '    InvokeMethod("OnDrawGizmos");', ""),

    # ---- 发布面 ---------------------------------------------------------------
    ("DGM16 the JS prototype has no no-op default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnDrawGizmos = function() {};"\n', ""),

    ("DGM17 the .d.ts declaration is gone", BINDING_CPP,
     '        "    OnDrawGizmos(): void;",\n', ""),

    ("DGM18 the .d.ts declaration is spelled differently", BINDING_CPP,
     '        "    OnDrawGizmosSelected(): void;",',
     '        "    OnDrawGizmosSelected() : void;",'),

    # ---- 主循环：实现了但没接进主循环 = 没实现，且静默 ----------------------
    # headless 下这一层测不到行为（m_CurrentActiveLevel 只能由真实加载流程设置），
    # 所以全靠 DW26..DW33 那几条源码级门。这几个变异体就是证明那些门会响。
    ("DGM20 the logical tick no longer drives the gizmo frame", APPLICATION_CPP,
     "    Application::DrawGizmosFrame();\n", ""),

    ("DGM21 the player skips nothing (the g_isEditor gate is gone)", APPLICATION_CPP,
     "void Application::DrawGizmosFrame",
     "    if (!g_isEditor)\n"
     "    {\n"
     "        return;\n"
     "    }\n", "", APP_RESOLVE),

    ("DGM22 the Instantiate table is skipped when resolving the selection",
     APPLICATION_CPP,
     "        if (GameObject* runtime_object = level->FindRuntimeGObject(id))\n"
     "        {\n"
     "            selected.push_back(runtime_object);\n"
     "        }\n", ""),

    # 解析不到的 ID 塞一个 nullptr 进去：下游 `is_selected` 会拿它比地址，
    # 而 `BroadcastDrawGizmos` 那边不会崩（只是多走一次），所以行为层看不出来
    # —— 只有 DG39 那条"缺席"的断言会红。
    ("DGM23 an unknown ID becomes a nullptr entry instead of being skipped",
     APPLICATION_CPP,
     "            selected.push_back(shared_object.get());\n"
     "            continue;\n",
     "            selected.push_back(shared_object.get());\n"
     "            continue;\n"
     "        }\n"
     "        if (true)\n"
     "        {\n"
     "            selected.push_back(nullptr);\n"),

    ("DGM24 the selection is not stored (SetGizmoSelection is a no-op)", APPLICATION_CPP,
     "    s_GizmoSelection = selected_ids;\n", ""),

    # 位置：挪到 LateTick **前面**。辅助线于是永远落后一帧 —— 只有 DW27 那条
    # 顺序断言能抓到。
    ("DGM25 the gizmo frame runs BEFORE LateTick", APPLICATION_CPP,
     ["    Application::DrawGizmosFrame();\n",
      "    GET_SYSTEM(WorldManager)->LateTick(delta_time);"],
     ["", "    Application::DrawGizmosFrame();\n"
          "    GET_SYSTEM(WorldManager)->LateTick(delta_time);"]),

    # ---- 门本身的门 ----------------------------------------------------------
    # DW6 是反向门，扫的是函数体里**没有** isEnabled。把切片退化成"半个文件"
    # 之后它变永假（Component.h 里当然有 isEnabled）—— 看起来像"有人把判据
    # 加回来了"，其实只是行尾变了。
    ("DGM19 the reader stops normalizing CRLF before slicing function bodies",
     PARITY_CPP,
     "        return NormalizeNewlines(ss.str());",
     "        return ss.str();"),
]

LOCK = os.path.join(BAK, "zengine_mutate_dg.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_dg.py is already running (%s)" % LOCK)
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
            # 备份**工作副本**（不是 HEAD）：这次的改动还没提交。
            bak = os.path.join(BAK, os.path.basename(path) + ".dgbak")
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
                with io.open(os.path.join(ROOT, ".mutate_dg.log"), "a", encoding="utf-8") as lg:
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
