"""Mutation harness for the LateUpdate / OnEnable / OnDisable slice.

What can rot silently here
--------------------------
  * A tick pass that is *implemented but never called*. Level::LateTick is 20
    lines of perfectly correct code that do nothing at all if
    Application::LogicalTick does not call it -- and there is no error.
  * The three tick passes sharing one enable predicate. Dropping a gate from
    LateTick does not crash; it just means "Update runs, LateUpdate doesn't"
    for one class of objects.
  * OnEnable/OnDisable being a *conjunction*. Implementing it as "enabled
    changed" or as "activeInHierarchy changed" alone both compile, both fire
    callbacks in the common case, and both miss a whole quadrant of the table.
  * Ordering. LateTick before Tick, or TickLateUpdate before LateTick, are each
    one line moved and each silently wrong.

Nothing here changes a number that is easy to eyeball; the tests have to read a
log, a count, or a source file.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

GO_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
LEVEL_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Level\Level.cpp")
WORLD_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\World\WorldManager.cpp")
APP_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Application\Application.cpp")
TS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
VM_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptVMSmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    """Whole-file first-occurrence replace (for strings that are unique)."""
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new):
    """Replace inside one function body only.

    Needed because the three tick passes in GameObject.cpp are textually
    identical; `mutate()` would always hit Tick (the first one) and never
    prove anything about LateTick.
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    j = text.find("\n}\n", i)
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
    print("  %-28s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:4]:
        print("      " + l.strip())
    return r.returncode


def build():
    last = None
    for _ in range(3):
        ok = True
        for target in ("UnityParitySmokeTest", "ScriptVMSmokeTest", "ScriptComponentSmokeTest"):
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
    if run(PARITY_EXE) | run(VM_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


GO_LATE = "void GameObject::LateTick(float delta_time)"
LEVEL_LATE = "void Level::LateTick(float delta_time)"
WORLD_LATE = "void WorldManager::LateTick(float delta_time)"
TS_REFRESH = "void TypeScriptComponent::RefreshEnabledState()"
TS_SETENABLED = "void TypeScriptComponent::SetEnabled(bool enabled)"
TS_OWNER = "void TypeScriptComponent::OnOwnerActiveChanged(bool active)"
TS_TEARDOWN = "void TypeScriptComponent::TearDown()"
TS_LATETICK = "void TypeScriptComponent::LateTick(float delta_time)"


MUTATIONS = [
    # ---- GameObject::LateTick 的三道门 ------------------------------------
    ("LM1 LateTick ignores activeInHierarchy", GO_CPP, GO_LATE,
     "    if (!IsActiveInHierarchy())\n    {\n        return;\n    }",
     ""),

    ("LM2 LateTick ignores the component's enabled flag", GO_CPP, GO_LATE,
     "if (component == nullptr || !component->isEnabled())",
     "if (component == nullptr)"),

    ("LM3 LateTick ignores the edit-mode whitelist", GO_CPP, GO_LATE,
     "if (!shouldComponentTick(component->GetTypeName()))",
     "if (false)"),

    ("LM4 LateTick dispatches Tick instead of LateTick", GO_CPP, GO_LATE,
     "component->LateTick(delta_time);",
     "component->Tick(delta_time);"),

    # ---- Level::LateTick --------------------------------------------------
    # 少了运行时表那一段，Instantiate 出来的对象就完全没有 LateUpdate —— 而关卡
    # 里摆的对象一切正常，所以只有"运行时生成的东西相机不跟随"这一种症状。
    ("LM5 Level::LateTick skips the runtime object table", LEVEL_CPP, LEVEL_LATE,
     "    for (const auto& id_object_pair : m_RuntimeGObjects)\n"
     "    {\n"
     "        if (id_object_pair.second != nullptr)\n"
     "        {\n"
     "            id_object_pair.second->LateTick(delta_time);\n"
     "        }\n"
     "    }",
     ""),

    ("LM6 Level::LateTick flushes destroys", LEVEL_CPP, LEVEL_LATE,
     "    GetTransformChangeDispatch().DispatchChanges();",
     "    FlushPendingDestroys();\n    GetTransformChangeDispatch().DispatchChanges();"),

    ("LM7 Level::LateTick forwards to Tick", LEVEL_CPP, LEVEL_LATE,
     "            id_object_pair.second->LateTick(delta_time);",
     "            id_object_pair.second->Tick(delta_time);"),

    # ---- WorldManager -----------------------------------------------------
    ("LM8 WorldManager::LateTick forwards to Tick", WORLD_CPP, WORLD_LATE,
     "        m_CurrentActiveLevel->LateTick(delta_time);",
     "        m_CurrentActiveLevel->Tick(delta_time);"),

    # ---- 调用点 -----------------------------------------------------------
    ("LM9 the frame loop never calls LateTick", APP_CPP, None,
     "    GET_SYSTEM(WorldManager)->LateTick(delta_time);\n",
     ""),

    # 把协程的 LateUpdate 相位挪到组件 LateUpdate 之前：只挪一行，症状是"等
    # LateUpdate 的协程拿到上一帧的相机位姿"。
    ("LM10 TickLateUpdate runs before LateTick", APP_CPP, None,
     "        coroutines->TickUpdate(ts.GetDeltaTime(), ts.GetUnscaledDeltaTime());\n"
     "    }",
     "        coroutines->TickUpdate(ts.GetDeltaTime(), ts.GetUnscaledDeltaTime());\n"
     "        coroutines->TickLateUpdate();\n"
     "    }"),

    # ---- 脚本转发 ---------------------------------------------------------
    ("LM11 TypeScriptComponent::LateTick calls OnUpdate", TS_CPP, TS_LATETICK,
     '"OnLateUpdate"',
     '"OnUpdate"'),

    # 合取退化成只看其中一个条件。两个变异互补：少了 activeInHierarchy 的那个
    # 在 C2m（SetActive(false)）露，少了 isEnabled 的那个在 C2j（enabled=false）露。
    ("LM12 OnEnable/OnDisable ignores activeInHierarchy", TS_CPP, TS_REFRESH,
     "const bool now = isEnabled() && (host != nullptr) && host->IsActiveInHierarchy();",
     "const bool now = isEnabled() && (host != nullptr);"),

    ("LM13 OnEnable/OnDisable ignores the enabled flag", TS_CPP, TS_REFRESH,
     "const bool now = isEnabled() && (host != nullptr) && host->IsActiveInHierarchy();",
     "const bool now = (host != nullptr) && host->IsActiveInHierarchy();"),

    ("LM14 SetEnabled no longer refreshes the state", TS_CPP, TS_SETENABLED,
     "    Super::SetEnabled(enabled);\n    RefreshEnabledState();",
     "    Super::SetEnabled(enabled);"),

    ("LM15 OnOwnerActiveChanged no longer refreshes the state", TS_CPP, TS_OWNER,
     "    Super::OnOwnerActiveChanged(active);\n    RefreshEnabledState();",
     "    Super::OnOwnerActiveChanged(active);"),

    # 少了这一块，OnDisable 只在"显式停用"时发，销毁路径上永远不发 —— 于是
    # 任何在 OnDisable 里退订 / 还池的脚本都会漏，且不报错、不崩溃。
    ("LM16 TearDown skips OnDisable before OnDestroy", TS_CPP, TS_TEARDOWN,
     "    if (sm && m_AwakeCalled && m_LastEnabledAndActive)\n"
     "    {\n"
     "        m_LastEnabledAndActive = false;\n"
     '        sm->InvokeInstanceMethod(ref, "OnDisable");\n'
     "    }\n",
     ""),

    # ---- 声明 / 兜底 ------------------------------------------------------
    ("LM17 the .d.ts stops declaring OnLateUpdate", BINDING_CPP, None,
     '        "    OnLateUpdate(delta_time: number): void;",\n',
     ""),

    ("LM18 the JS Behaviour loses its OnLateUpdate default", ENV_CPP, None,
     '        "  g.Behaviour.prototype.OnLateUpdate = function(dt) {};"\n',
     ""),

    ("LM19 the JS Behaviour loses its OnEnable default", ENV_CPP, None,
     '        "  g.Behaviour.prototype.OnEnable = function() {};"\n',
     ""),

    ("LM20 the JS Behaviour loses its OnDisable default", ENV_CPP, None,
     '        "  g.Behaviour.prototype.OnDisable = function() {};"\n',
     ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_lu.lock")


def main():
    # 两个 harness 同时跑会互相踩 MSBuild（MSB3371 + LNK1104），还都往同一个
    # log 写。用锁文件挡住第二个实例。
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_lu.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    try:
        for name, path, func_sig, old, new in MUTATIONS:
            if only and name.split()[0] != only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".lubak")
            subprocess.run(["git", "show", "HEAD:" + os.path.relpath(path, ROOT).replace("\\", "/")],
                           cwd=ROOT, env=ENV, stdout=io.open(bak, "wb"), stderr=subprocess.DEVNULL)
            try:
                if func_sig is None:
                    mutate(path, old, new)
                else:
                    mutate_in(path, func_sig, old, new)
                verdict = "BUILD FAILED"
                for _ in range(3):
                    verdict = build_and_run()
                    if verdict != "BUILD FAILED":
                        break
                    print("  (retrying -- flaky MSBuild)")
                if verdict == "BUILD FAILED":
                    verdict = "invalid (does not build)"
                results.append((name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        print("---- summary ----")
        for n, r in results:
            print("%-58s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
