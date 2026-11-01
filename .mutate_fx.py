"""Mutation harness for publishing `OnFixedUpdate` (the PU / FU / T slice).

Why this slice needs its own harness -- and why it is shaped differently from
the other ones:

* The bug it fixes was "the engine calls it, the script cannot see it". C++ is
  structurally blind to that class of bug: `InvokeInstanceMethodNumber` treats a
  missing method as a silent no-op success, so every existing C++ test stayed
  green while `OnFixedUpdate` was absent from the .d.ts (and therefore
  unusable from TypeScript -- `npx tsc` would reject it).
* So the fix is not "add one more assertion"; it is a GENERIC gate (PU) that
  extracts every method name `TypeScriptComponent.cpp` invokes on the script
  instance and demands it appear in both `ScriptBinding.cpp` and `ScriptEnv.cpp`.
  FX01 / FX02 prove that gate actually fires for the name that was missing.
* FX10 / FX11 are the gate-for-the-gate: FX10 drops the "must be a valid
  identifier" filter (comment mentions of `InvokeInstanceMethod` then drag in a
  log-message string and turn into assertions that can never be satisfied), and
  FX11 breaks one of the two scan anchors. Without PU3's floor, a scan that
  silently finds nothing is indistinguishable from a codebase with no hooks.
* FX13 / FX14 are the "is the fixture vacuous?" mutants: FX13 removes the
  `g_isPlaying = true` the T block needs (proving T0 is not passing because
  nothing is dispatched at all), and FX14 switches the calls from `go2->` to
  `comp2->`, bypassing the enabled / active gates that live in GameObject --
  which is exactly the shape a future edit is most likely to take.

Run:  py .mutate_fx.py            (all)
      py .mutate_fx.py FX01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

GAMEOBJECT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\BaseClasses\GameObject.cpp")
TS_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.cpp")
ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
PARITY_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Test\UnityParitySmokeTest.cpp")
SCRIPTCOMP_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Scripting\ScriptVM\Test\ScriptComponentSmokeTest.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_all(path, old, new):
    """替换**全部**出现处。

    ⚠️ FX14 第一次只换了第一处就 NOT CAUGHT：T 段里 `go2->FixedTick(fixed_step);`
    出现四次（T0 / T4 / T5 / T7），而只有换掉**判据那两处**（T4 / T5）才碰得到
    "绕过 GameObject 的 enabled / 激活判据"。只换 T0 那处，派发照常发生，T0 依旧
    是绿的 —— 变异体等于没写。
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new))


def mutate_in(path, func_sig, old, new, end_marker):
    """Replace only inside one function body (see .mutate_dg.py for why the
    default `\\n    }\\n` anchor is unusable here: both FixedTick bodies open
    with an `if (...) { return; }` that matches it first)."""
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    j = text.find(end_marker, i)
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
    if (run(PARITY_EXE) | run(COMP_EXE)) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


# ---- 锚点 ------------------------------------------------------------------
GO_FIXED_END = "\nvoid GameObject::LateTick"
TS_FIXED_END = "\nvoid TypeScriptComponent::LateTick"

ENABLED_GATE = (
    "        if (component == nullptr || !component->isEnabled())\n"
    "        {\n"
    "            continue;\n"
    "        }\n")

ACTIVE_GATE = (
    "    if (!IsActiveInHierarchy())\n"
    "    {\n"
    "        return;\n"
    "    }\n")

WHITELIST_GATE = (
    "        if (!shouldComponentTick(component->GetTypeName()))\n"
    "        {\n"
    "            continue;\n"
    "        }\n")

FIXED_FORWARD = (
    '    sm->InvokeInstanceMethodNumber(ref, "OnFixedUpdate", '
    'static_cast<double>(fixed_delta_time));')

MUTATIONS = [
    # ---- ⚠️ 头条：就是这个洞 ------------------------------------------------
    # d.ts 里没有 `OnFixedUpdate` ⇒ TS 里写它过不了 tsc，而任何 C++ 测试都绿。
    ("FX01 the .d.ts declaration of OnFixedUpdate is gone", BINDING_CPP,
     '        "    OnFixedUpdate(fixed_delta_time: number): void;",\n', ""),

    ("FX02 the JS prototype has no OnFixedUpdate default", ENV_CPP,
     '        "  g.Behaviour.prototype.OnFixedUpdate = function(dt) {};"\n', ""),

    # ---- 转发：三个 Tick 长得一模一样，抄错是最可能的错 ----------------------
    ("FX03 TypeScriptComponent::FixedTick forwards OnUpdate (copy-paste slip)",
     TS_CPP, "void TypeScriptComponent::FixedTick",
     FIXED_FORWARD,
     '    sm->InvokeInstanceMethodNumber(ref, "OnUpdate", '
     'static_cast<double>(fixed_delta_time));',
     TS_FIXED_END),

    ("FX04 TypeScriptComponent::FixedTick forwards nothing at all", TS_CPP,
     "void TypeScriptComponent::FixedTick", FIXED_FORWARD, "", TS_FIXED_END),

    # ---- 判据：两条循环必须同构 ---------------------------------------------
    # 少了任何一条就是"Update 停了但 FixedUpdate 还在推"那一类错位。
    ("FX05 GameObject::FixedTick drops the enabled gate", GAMEOBJECT_CPP,
     "void GameObject::FixedTick", ENABLED_GATE, "", GO_FIXED_END),

    ("FX06 GameObject::FixedTick drops the activeInHierarchy gate", GAMEOBJECT_CPP,
     "void GameObject::FixedTick", ACTIVE_GATE, "", GO_FIXED_END),

    # ⚠️ 这条只能靠 T7（"非播放态下必须什么都不发"）看出来：没有 T7 的话，
    # "FixedTick 不过白名单"在播放态下完全正常。
    ("FX07 GameObject::FixedTick drops the edit-mode whitelist", GAMEOBJECT_CPP,
     "void GameObject::FixedTick", WHITELIST_GATE, "", GO_FIXED_END),

    # 步长传错：传成 Tick 的话计数照样 +1，只有 T3（两个 delta 必须不同）能看见。
    ("FX08 FixedTick calls Tick instead (the step value is then the frame delta)",
     GAMEOBJECT_CPP, "void GameObject::FixedTick",
     "        component->FixedTick(fixed_delta_time);",
     "        component->Tick(fixed_delta_time);",
     GO_FIXED_END),

    # ---- 门本身的门 ----------------------------------------------------------
    # 过滤掉"非标识符"之后，注释里提到 InvokeInstanceMethod 就不再拖进日志文案。
    # 去掉它 ⇒ 凭空多出两条永远满足不了的断言（看起来像"有人忘了发布"）。
    ("FX10 the publishing gate stops filtering non-identifiers out", PARITY_CPP,
     "                if (is_identifier(name) &&",
     "                if (true &&"),

    # 少扫一个锚点 ⇒ InvokeInstanceMethod* 那一族整族漏掉。只有 PU3 那条
    # "扫描至少捞到 18 个"的下限能看见（剩下的那些每条都还是绿的）。
    ("FX11 the publishing gate loses the InvokeInstanceMethod anchor", PARITY_CPP,
     '        for (const auto& extra : collect(ts_source, "InvokeInstanceMethod"))',
     '        for (const auto& extra : collect(ts_source, "InvokeInstanceMethodZZZ"))'),

    # ---- 夹具是不是空的 -------------------------------------------------------
    # 忘了进播放态 ⇒ 什么都不会派发。T0 必须红，否则说明它本来就是空转的。
    ("FX13 the T block forgets to enter play mode (so nothing is dispatched)",
     SCRIPTCOMP_CPP, "        g_isPlaying = true;\n", ""),

    # ⚠️ 最可能被后人改成的样子：直接调组件。enabled / 层级激活那两道判据活在
    # GameObject 里，绕过它们之后 T4 / T5 就永远绿了。
    ("FX14 the T block calls the component instead of the GameObject", SCRIPTCOMP_CPP,
     "go2->FixedTick(fixed_step);", "comp2->FixedTick(fixed_step);", "ALL"),
]

LOCK = os.path.join(BAK, "zengine_mutate_fx.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_fx.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            replace_all = False
            if len(entry) == 6:
                name, path, func_sig, old, new, end_marker = entry
            elif len(entry) == 5 and entry[4] == "ALL":
                name, path, old, new = entry[0], entry[1], entry[2], entry[3]
                func_sig = None
                end_marker = None
                replace_all = True
            else:
                name, path, old, new = entry
                func_sig = None
                end_marker = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".fxbak")
            shutil.copy2(path, bak)
            try:
                try:
                    if func_sig is not None:
                        mutate_in(path, func_sig, old, new, end_marker)
                    elif replace_all:
                        mutate_all(path, old, new)
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
                with io.open(os.path.join(ROOT, ".mutate_fx.log"), "a", encoding="utf-8") as lg:
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
