"""Mutation harness for `Renderer` bounds + `OnBecameVisible` / `OnBecameInvisible` (the RV / RW / V slice).

Why this slice needs its own harness at all:

* Visibility is a GLOBAL scan (every renderer x every camera) driven from the
  main loop, not from `BaseRenderer::Tick`. A renderer that never registers
  itself, or a camera that never registers itself, produces exactly the same
  observable as "nothing is visible" -- RVM12 / RVM20 are those two mutants,
  and RV6 is the assertion that must fire.
* Two of the three gates in `UpdateVisibilityAndNotify` (`isEnabled`,
  `IsActiveInHierarchy`) each have their OWN assertion (RV14, RV17), because
  removing either one leaves "the scan still runs" intact. And the third gate
  -- the one in `BroadcastVisibilityChange` -- is invisible to `IsVisible()`
  entirely (that value is computed before the broadcast), which is why RV17b
  had to be added: it asserts the message is NOT sent.
* `GetBounds()` transforming all eight corners is only observable with an
  ASYMMETRIC box under rotation (RV5's (4,2,2) -> (2,4,2)). A cube-based
  fixture cannot see RVM05.
* The publishing surface (`.d.ts` + the prototype no-op default) has no
  runtime observable at all in this repo -- `Mover.ts` always defines the
  hooks. RW10 / RW12 are the source-level gates, and RVM14 / RVM15 are what
  prove they fire.

Known coverage gaps (deliberately NOT written as mutants -- a mutant that
cannot be caught is self-deception, see .mutate_dg.py's DGM13):

* `UpdateVisibilityForAll()` copies the registry before iterating so a script
  may destroy objects from inside `OnBecameVisible`. No fixture here mutates
  the registry from a callback, so "iterate the live vector" would survive.
* `~BaseRenderer` unregistering from the registry is likewise unobservable:
  nothing in these tests destroys a renderer while the scan can still run
  afterwards.

Run:  py .mutate_rv.py            (all)
      py .mutate_rv.py RVM03      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

GAMEOBJECT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
RENDERER_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Mesh\BaseRenderer.cpp")
RENDERER_H = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Mesh\BaseRenderer.h")
CAMERA_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Camera\CameraComponent.cpp")
FRUSTUM_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\Frustum.cpp")
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

    ⚠️ 默认锚点 `"\\n    }\\n"` 只定位**第一个**缩进 4 的收尾花括号，而
    `UpdateVisibilityAndNotify` / `BroadcastVisibilityChange` 里都有更早的
    `if (...) { return; }`。这些函数**必须**显式给 `end_marker`。
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


# --- 锚点 -------------------------------------------------------------------
GO_END = "\n// Both lookups go through"
VIS_END = "\nvoid BaseRenderer::UpdateVisibilityForAll"
GETBOUNDS_END = "\nvoid BaseRenderer::UpdateVisibilityAndNotify"
LOCAL_END = "\nvoid BaseRenderer::SetLocalBounds"
SETENABLED_END = "\nvoid BaseRenderer::Tick"
ACTIVECHANGED_END = "\nvoid BaseRenderer::SetEnabled"
FRUSTUM_END = "\nbool Frustum::HasDegeneratePlane"
CAMERA_END = "\nconst std::vector<CameraComponent*>& CameraComponent::GetAllCameraComponents"

VIS_BODY = (
    "        if (visible)\n"
    "        {\n"
    "            component->OnBecameVisible();\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "            component->OnBecameInvisible();\n"
    "        }\n")

ACTIVE_GATE = (
    "    if (!IsActiveInHierarchy())\n"
    "    {\n"
    "        return;\n"
    "    }\n")

UPDATE_GATE = ("    if (isEnabled() && m_ParentObject != nullptr && "
               "m_ParentObject->IsActiveInHierarchy())\n")

TRANSITION_GATE = (
    "    if (visible == m_VisibleLastFrame)\n"
    "    {\n"
    "        return;\n"
    "    }\n")

MUTATIONS = [
    # ---- ⚠️ 头条一："顺手把 enabled 也判上" ----------------------------------
    # Unity 明文：enabled 只挡 Start/Awake/Update/FixedUpdate/OnGUI。可见性不在
    # 其中。这个变异体看起来像修 bug，是最可能真被写进去的一个 —— RV16（运行时）
    # 与 RW8（源码反向门）两道都得红。
    ("RVM01 someone 'fixes' it by also gating the broadcast on enabled", GAMEOBJECT_CPP,
     "void GameObject::BroadcastVisibilityChange",
     VIS_BODY,
     "        if (!component->isEnabled())\n"
     "        {\n"
     "            continue;\n"
     "        }\n" + VIS_BODY,
     GO_END),

    # 层级激活判据被删：停用的对象也会被通知。只有 RV17b 能看见 —— RV17 读的是
    # `IsVisible()`，那个值在广播**之前**就算好了。
    ("RVM02 the broadcast's activeInHierarchy gate is gone", GAMEOBJECT_CPP,
     "void GameObject::BroadcastVisibilityChange", ACTIVE_GATE, "", GO_END),

    # ---- 头条二：可见性自身的三条判据 --------------------------------------
    ("RVM03 UpdateVisibilityAndNotify stops asking isEnabled", RENDERER_CPP,
     "void BaseRenderer::UpdateVisibilityAndNotify",
     UPDATE_GATE,
     "    if (m_ParentObject != nullptr && m_ParentObject->IsActiveInHierarchy())\n",
     VIS_END),

    ("RVM04 UpdateVisibilityAndNotify stops asking IsActiveInHierarchy", RENDERER_CPP,
     "void BaseRenderer::UpdateVisibilityAndNotify",
     UPDATE_GATE,
     "    if (isEnabled() && m_ParentObject != nullptr)\n",
     VIS_END),

    # 去掉"只在变化那一帧"：每帧都发。RV8 是那条断言。
    ("RVM05 the transition gate is gone (it fires every frame)", RENDERER_CPP,
     "void BaseRenderer::UpdateVisibilityAndNotify", TRANSITION_GATE, "", VIS_END),

    # ---- 包围盒 --------------------------------------------------------------
    # 只变换两个角：非均匀盒 + 旋转下算出来的盒比真的小。RV5（(4,2,2) 绕 Z 转
    # 90° 应为 (2,4,2)）是唯一能看见的地方 —— 立方体夹具看不出来。
    ("RVM06 GetBounds transforms only two corners", RENDERER_CPP,
     "Bounds BaseRenderer::GetBounds() const",
     "    for (int corner = 0; corner < 8; ++corner)",
     "    for (int corner = 0; corner < 2; ++corner)",
     GETBOUNDS_END),

    # 默认回退成单位立方体而不是零盒。"没有网格资产"于是看起来像"一米见方"，
    # 而 headless 下**永远**没有网格资产 —— 所有测试都带着这个错值跑。
    ("RVM07 the no-mesh fallback becomes a unit cube", RENDERER_CPP,
     "Bounds BaseRenderer::GetLocalBounds() const",
     "any ? Bounds::FromMinMax(min_corner, max_corner) : Bounds()",
     "any ? Bounds::FromMinMax(min_corner, max_corner) "
     ": Bounds::FromCenterSize(Vector3::ZERO, Vector3(1.f, 1.f, 1.f))",
     LOCAL_END),

    # `IsVisible` 恒真：可见性于是从没被算过，但所有"应该可见"的断言都照绿。
    ("RVM08 IsVisible is hardcoded true", RENDERER_H,
     "bool IsVisible() const { return m_VisibleLastFrame; }",
     "bool IsVisible() const { return true; }"),

    # ---- 两张注册表 ----------------------------------------------------------
    # 渲染器不注册 ⇒ 扫描永远扫不到任何东西，"没有渲染器"和"渲染器不可见"
    # 长得一模一样。RV6 必须红。
    ("RVM09 BaseRenderer never registers itself", RENDERER_CPP,
     "BaseRenderer::BaseRenderer()",
     "    BaseRendererRegistry::Get().renderers.push_back(this);", ""),

    # 相机不注册 ⇒ 同理，一台相机都没有。
    ("RVM10 CameraComponent never registers itself", CAMERA_CPP,
     "CameraComponent::CameraComponent()",
     "    CameraComponentRegistry::Get().Add(this);", ""),

    # ---- 相机的判据 ----------------------------------------------------------
    ("RVM11 a disabled camera still makes things visible", CAMERA_CPP,
     "bool CameraComponent::CanSeeAnything() const",
     "    if (!isEnabled())\n"
     "    {\n"
     "        return false;\n"
     "    }\n", "", CAMERA_END),

    # ---- 视锥 ----------------------------------------------------------------
    ("RVM12 Frustum::Intersects always returns true", FRUSTUM_CPP,
     "bool Frustum::Intersects",
     "        if (m_Planes[i].GetDistanceToPoint(positive_vertex) < 0.f)\n"
     "        {\n"
     "            return false;\n"
     "        }\n", "", FRUSTUM_END),

    # ---- "变化"不一定发生在扫描那一刻 ----------------------------------------
    # 关掉渲染器：这一帧之后 Tick 已经不会再来了（transform 不脏时 Tick 早退），
    # 而扫描是每帧的 —— 于是只有 SetEnabled 里那次立刻刷新能让它当帧生效。
    ("RVM13 SetEnabled no longer refreshes visibility", RENDERER_CPP,
     "void BaseRenderer::SetEnabled(bool enabled)",
     "    // 同上：停用的渲染器不渲染 ⇒ 不可见，而这一帧之后 Tick 已经不会再来了。\n"
     "    UpdateVisibilityAndNotify();\n", "", SETENABLED_END),

    # 层级变化同理：对象未激活时 GameObject::Tick 直接 return。RV18（不先扫一帧
    # 就断言）是那条门。
    ("RVM14 OnOwnerActiveChanged no longer refreshes visibility", RENDERER_CPP,
     "void BaseRenderer::OnOwnerActiveChanged(bool active)",
     "    // ⚠️ 可见性必须在这里也刷一次：层级一变，Tick 可能就再也不会被调到（对象\n"
     "    // 未激活时 GameObject::Tick 直接返回），可见性会永远停在旧值上。\n"
     "    UpdateVisibilityAndNotify();\n", "", ACTIVECHANGED_END),

    # ---- 主循环：实现了但没接进主循环 = 没实现，且静默 ----------------------
    ("RVM15 the main loop does not drive the visibility scan", APPLICATION_CPP,
     "    BaseRenderer::UpdateVisibilityForAll();\n", ""),

    # ---- 脚本侧转发 ----------------------------------------------------------
    # 名字只差一个词：串了的话脚本里表现为"进出视锥同时记 +1"，两个回调都在被
    # 调到，没有任何报错。V3（字母**顺序**）是那条门。
    ("RVM16 the script component forwards the wrong hook name", TS_CPP,
     '    InvokeMethod("OnBecameVisible");',
     '    InvokeMethod("OnBecameInvisible");'),

    # ---- 发布面 ---------------------------------------------------------------
    # 只有源码级门（RW10 / RW12）能看见：Mover.ts 自己定义了这两个钩子，所以
    # 运行期一切正常，坏的是**用户工程**里那些没定义的类。
    ("RVM17 the .d.ts declaration is gone", BINDING_CPP,
     '        "    OnBecameVisible(): void;",\n', ""),

    ("RVM18 the JS prototype has no no-op default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnBecameVisible = function() {};"\n', ""),

    # ---- 门本身的门 ----------------------------------------------------------
    # RW6/RW7/RW8 切的是 `BroadcastVisibilityChange` 的**函数体**。不先归一化
    # CRLF 的话，`"\n    }\n"` 锚不稳，切片会退化成"半个文件" —— 那时 RW8
    # （函数体里不该出现 isEnabled）变永假，看起来像"判据还在"，其实只是行尾
    # 变了。RVM19 就是证明那条归一化真的在生效。
    ("RVM19 the reader stops normalizing CRLF before slicing function bodies",
     PARITY_CPP,
     "        return NormalizeNewlines(ss.str());",
     "        return ss.str();"),
]

LOCK = os.path.join(BAK, "zengine_mutate_rv.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_rv.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".rvbak")
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
                with io.open(os.path.join(ROOT, ".mutate_rv.log"), "a", encoding="utf-8") as lg:
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
