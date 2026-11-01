"""Mutation harness for the Unity application-message slice.

Two families of mutants here:

* "the message never arrives"      -> caught by AM*/P* (behaviour).
* "the gate came back"             -> caught by AW* (source-level scan of the
  function BODY). AW8/AW9/AW13 exist precisely because a behaviour test cannot
  see them: the probe is enabled and active by default, so re-adding
  `isEnabled()` to the walk leaves every behavioural assertion green.

AM12 (bool -> 1.0/0.0) is the reason Mover.ts writes `=== true` instead of
`if (hasFocus)`: with implicit truthiness that mutant survives.
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
APP_H = os.path.join(ROOT, r"engine\Source\Runtime\Application\Application.h")
APP_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Application\Application.cpp")
TRANSFORM_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Transform\Transform.cpp")
TS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.cpp")
ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
VM_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptVMSmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


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
    """Replace only inside `func_sig`'s body.

    锚点必须限定在函数内：`if (m_WorldPartition.IsEnabled()) { for (auto&
    level_pair : m_LoadedLevels)` 在 WorldManager.cpp 里出现 **4 次**，只做全局
    replace(old, new, 1) 会改到前面另一个函数上 —— 于是得到一个"什么也没改到"
    的变异体，报 SURVIVED（AM6 第一次就是这么存活的）。
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


MUTATIONS = [
    # ---- GameObject 那一趟遍历 ----------------------------------------------
    # 把 Tick 的判据加回来：被停用的对象于是收不到"应用要退出了"。
    ("AM1 the walk gates on isEnabled after all", GAMEOBJECT_CPP,
     "        switch (kind)\n"
     "        {\n"
     "            case AppEventKind::kFocus:",
     "        if (!component->isEnabled())\n"
     "        {\n"
     "            continue;\n"
     "        }\n"
     "        switch (kind)\n"
     "        {\n"
     "            case AppEventKind::kFocus:"),

    # kPause 转给了错误的虚函数：OnApplicationPause 于是永远没人调。
    ("AM2 kPause is dispatched to OnApplicationFocus", GAMEOBJECT_CPP,
     "            case AppEventKind::kPause:\n"
     "                component->OnApplicationPause(arg);",
     "            case AppEventKind::kPause:\n"
     "                component->OnApplicationFocus(arg);"),

    ("AM3 kQuit has no dispatch at all", GAMEOBJECT_CPP,
     "            case AppEventKind::kQuit:\n"
     "                component->OnApplicationQuit();\n"
     "                break;\n",
     ""),

    # 只发给第一个组件：只有一个组件的测试完全看不出来（AM7 / AM8 是判据）。
    ("AM4 the walk breaks after the first component", GAMEOBJECT_CPP,
     "            case AppEventKind::kParentChanged:\n"
     "                component->OnTransformParentChanged();\n"
     "                break;\n"
     "        }\n"
     "    }",
     "            case AppEventKind::kParentChanged:\n"
     "                component->OnTransformParentChanged();\n"
     "                break;\n"
     "        }\n"
     "        break;  // mutant: only the first component\n"
     "    }"),

    # ---- Level：两张表 -------------------------------------------------------
    ("AM5 Level forgets the runtime object table", LEVEL_CPP,
     "    for (const auto& id_object_pair : m_RuntimeGObjects)\n"
     "    {\n"
     "        if (id_object_pair.second != nullptr)\n"
     "        {\n"
     "            id_object_pair.second->BroadcastAppEvent(kind, arg);\n"
     "        }\n"
     "    }\n"
     "}\n"
     "\n"
     "void Level::LateTick(float delta_time)",
     "}\n"
     "\n"
     "void Level::LateTick(float delta_time)"),

    # ---- WorldManager：早退 -------------------------------------------------
    # 行为上观测不到（单元测试里没有已加载的世界），只有 AW13 这个函数体扫描
    # 能抓到 —— 这正是 AW13 存在的理由。
    ("AM6 WorldManager early-returns when the world is not loaded", WORLD_CPP,
     "void WorldManager::BroadcastAppEvent(AppEventKind kind, bool arg)",
     "    if (m_WorldPartition.IsEnabled())",
     "    if (!m_IsWorldLoaded)\n"
     "    {\n"
     "        return;\n"
     "    }\n"
     "    if (m_WorldPartition.IsEnabled())"),

    # ---- pause 的推导 --------------------------------------------------------
    # Pause 退化成 Focus 的别名：runInBackground=true 时失焦也会"暂停"。
    ("AM7 Pause becomes Focus's alias", APP_H,
     "        return !has_focus && !run_in_background;",
     "        return !has_focus;"),

    # 反过来：只看 runInBackground，有焦点也算暂停。
    ("AM8 Pause ignores focus entirely", APP_H,
     "        return !has_focus && !run_in_background;",
     "        return !run_in_background;"),

    # ---- 退出通知的时机 ------------------------------------------------------
    ("AM9 the quit broadcast moves after RequestClose", APP_CPP,
     "    if (auto* world = GET_SYSTEM(WorldManager))\n"
     "    {\n"
     "        world->BroadcastAppEvent(AppEventKind::kQuit, false);\n"
     "    }\n",
     ""),

    # ---- 改父级 ---------------------------------------------------------------
    ("AM10 SetParent never notifies", TRANSFORM_CPP,
     "    if (GameObject* const owner = GetParentObject())\n"
     "    {\n"
     "        owner->BroadcastAppEvent(AppEventKind::kParentChanged, false);\n"
     "    }\n",
     ""),

    # 早一步派发（搬到 SetParent 开头）：回调照样被调一次（P7 绿），但回调里
    # 读 `transform.parent` 读到的还是**旧**父级 —— P7b 是那条判据。
    ("AM11 the parent-change broadcast happens before the hierarchy is updated",
     TRANSFORM_CPP,
     ["    if (GameObject* const owner = GetParentObject())\n"
      "    {\n"
      "        owner->BroadcastAppEvent(AppEventKind::kParentChanged, false);\n"
      "    }\n",
      "    Transform* const old_father = m_Father;\n"
      "    if (old_father == new_parent)"],
     ["",
      "    if (GameObject* const owner = GetParentObject())\n"
      "    {\n"
      "        owner->BroadcastAppEvent(AppEventKind::kParentChanged, false);\n"
      "    }\n"
      "    Transform* const old_father = m_Father;\n"
      "    if (old_father == new_parent)"]),

    # ---- 脚本侧转发 -----------------------------------------------------------
    ("AM12 the focus arg is hardcoded to true", TS_CPP,
     '    InvokeMethodBool("OnApplicationFocus", has_focus);',
     '    InvokeMethodBool("OnApplicationFocus", true);'),

    # 用无参版本转发：JS 收到 undefined，`pauseStatus === true` 为假。
    ("AM13 OnApplicationPause is forwarded without its bool", TS_CPP,
     '    InvokeMethodBool("OnApplicationPause", paused);',
     '    InvokeMethod("OnApplicationPause");'),

    ("AM14 OnValidate forgets the editor gate", TS_CPP,
     "    if (g_isEditor)\n"
     "    {\n"
     "        InvokeMethod(\"OnValidate\");\n"
     "    }",
     "    InvokeMethod(\"OnValidate\");"),

    ("AM15 OnSerializedFieldsUpdated stops forwarding", TS_CPP,
     '        InvokeMethod("OnValidate");',
     ""),

    # ---- bool 必须是真 bool ---------------------------------------------------
    # 这是 `Mover.ts` 里那两个 `=== true` 唯一的作用。
    ("AM16 the bool arg is created as a number", ENV_CPP,
     "        pesapi_value arg_val = pesapi_create_boolean(m_Papis, env, bool_arg ? 1 : 0);",
     "        pesapi_value arg_val = pesapi_create_double(m_Papis, env, bool_arg ? 1.0 : 0.0);"),

    # ---- 声明 ------------------------------------------------------------------
    ("AM17 the .d.ts drops OnApplicationFocus", BINDING_CPP,
     '        "    OnApplicationFocus(has_focus: boolean): void;",\n',
     ""),

    ("AM18 the JS prototype has no OnApplicationQuit default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnApplicationQuit = function() {};"\n',
     ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_am.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_am.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            if len(entry) == 5:
                # (name, path, func_sig, old, new)：只在那个函数体内替换。
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".ambak")
            subprocess.run(["git", "show", "HEAD:" + os.path.relpath(path, ROOT).replace("\\", "/")],
                           cwd=ROOT, env=ENV, stdout=io.open(bak, "wb"), stderr=subprocess.DEVNULL)
            try:
                if func_sig is not None:
                    mutate_in(path, func_sig, old, new)
                else:
                    mutate(path, old, new)
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
            print("%-64s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
