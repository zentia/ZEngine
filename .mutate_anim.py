"""Mutation harness for the Animator completion slice (#54).

What is under test
------------------
`AnimatorInstance` grew a global playback `speed`, the controller-shape queries
(parameter/state counts and names), `IsCurrentState`, and `Rebind`.
`AnimationComponent` forwards all of it plus Unity's `GetInteger`/`SetInteger`
spelling, `Update(dt)` and the fired-event list.

Why these particular mutants
----------------------------
* **speed is a multiplier, so the mutant has to be a multiplier too.** AM02
  drops it from Tick and AM01 stops clamping negatives; both are caught only
  because the test asserts an exact normalized time (0.25 then 0.75 on a 2s
  clip) rather than "the playhead moved".
* **Counts and names are the pair that lets a script *enumerate* instead of
  guessing.** Swapping parameter count for state count (AM03) is invisible in a
  controller that happens to have two of each -- the fixture deliberately has
  two of each, and the *names* ("speed"/"combo" vs "Idle"/"Run") are what
  separate them. AM05/AM06/AM18 cover the name side.
* **Rebind has two halves and both need killing.** It must reset the playhead
  *and* restore parameter defaults; AM09 removes the whole thing and BN27/BN28
  each pin one half.
* **GetInteger/SetInteger are Unity's spelling over the engine's GetInt/SetInt.**
  AM13/AM14 make one of the two spellings diverge, which is exactly the bug you
  get when an alias is added and later the original is changed without touching
  it.
* **The fired-event list is cleared at the start of every advance.** AM16/AM17
  make it permanently empty; BN25 catches that, and BN26 pins the "no
  double-read" half.

Run:  py .mutate_anim.py            (all)
      py .mutate_anim.py AM01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


INST_H = S(r"engine/Source/Runtime/Function/Animation/AnimatorInstance.h")
INST_CPP = S(r"engine/Source/Runtime/Function/Animation/AnimatorInstance.cpp")
COMP_H = S(r"engine/Source/Runtime/Function/Framework/Component/Animation/AnimationComponent.h")
COMP_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Animation/AnimationComponent.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# .cpp 里的目标函数在文件作用域，函数收尾是顶格的 `}`。
END_OF_FUNC = "\n}\n"
# ⚠️ 类里的内联函数：函数体 8 空格，收尾是 4 空格的 `}`。
END_OF_CLASS_FUNC = "\n    }\n"


def marker_for(path):
    return END_OF_CLASS_FUNC if path.endswith((".h",)) else END_OF_FUNC


TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else marker_for(path)
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
                               env=ENV, capture_output=True, text=True, timeout=1800)
            last = p
            if p.returncode != 0:
                ok = False
                if "LNK1236" in (p.stdout or ""):
                    subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime",
                                    "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                                   env=ENV, capture_output=True, text=True, timeout=1800)
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
    # =========================================================================
    # speed
    # =========================================================================
    ("AM01 a negative speed is accepted (no clamp)",
     INST_H, None,
     "void SetSpeed(float speed) { m_Speed = speed > 0.0f ? speed : 0.0f; }",
     "void SetSpeed(float speed) { m_Speed = speed; }"),

    ("AM02 Tick ignores speed",
     INST_CPP, "void AnimatorInstance::Tick(float delta_time)",
     "    const float scaled_delta = delta_time * m_Speed;",
     "    const float scaled_delta = delta_time;"),

    # =========================================================================
    # 控制器形状（计数与名字）
    # =========================================================================
    ("AM03 GetParameterCount reports the state count",
     INST_CPP, "int AnimatorInstance::GetParameterCount() const",
     "m_Controller->parameters.size()", "m_Controller->states.size()"),

    ("AM04 GetStateCount reports the parameter count",
     INST_CPP, "int AnimatorInstance::GetStateCount() const",
     "m_Controller->states.size()", "m_Controller->parameters.size()"),

    ("AM05 GetParameterName always returns empty",
     INST_CPP, "const char* AnimatorInstance::GetParameterName(int index) const",
     '    return m_Controller->parameters[static_cast<size_t>(index)].name.c_str();',
     '    return "";'),

    ("AM06 GetStateName always returns empty",
     INST_CPP, "const char* AnimatorInstance::GetStateName(int index) const",
     '    return m_Controller->states[static_cast<size_t>(index)].name.c_str();',
     '    return "";'),

    # ⚠️ 把参数名换成状态名：两边都是 2 个，只数个数分不出来，只有名字分得出。
    ("AM18 GetParameterName reads the state list",
     INST_CPP, "const char* AnimatorInstance::GetParameterName(int index) const",
     "m_Controller->parameters[static_cast<size_t>(index)].name",
     "m_Controller->states[static_cast<size_t>(index)].name"),

    # =========================================================================
    # 当前状态判定
    # =========================================================================
    ("AM07 IsCurrentState asks about the transition target instead",
     INST_CPP, "bool AnimatorInstance::IsCurrentState(const char* state_name) const",
     "m_Controller->states[static_cast<size_t>(m_CurrentState)].name == state_name",
     "m_Controller->states[static_cast<size_t>(m_TransitionTarget)].name == state_name"),

    ("AM08 IsCurrentState always says yes",
     INST_CPP, "bool AnimatorInstance::IsCurrentState(const char* state_name) const",
     '    return m_Controller->states[static_cast<size_t>(m_CurrentState)].name == state_name;',
     '    return true;'),

    # =========================================================================
    # Rebind
    # =========================================================================
    ("AM09 Rebind is a no-op",
     INST_H, None,
     "void Rebind() { Reset(); }", "void Rebind() { }"),

    # =========================================================================
    # 组件层转发
    # =========================================================================
    ("AM10 GetSpeed always reports 1",
     COMP_H, None,
     "float GetSpeed() const { return m_Animator.GetSpeed(); }",
     "float GetSpeed() const { return 1.0f; }"),

    ("AM11 GetParameterName always returns the first parameter",
     COMP_H, None,
     "eastl::string GetParameterName(int index) const { return eastl::string(m_Animator.GetParameterName(index)); }",
     "eastl::string GetParameterName(int index) const { return eastl::string(m_Animator.GetParameterName(0)); }"),

    ("AM12 GetStateName always returns the first state",
     COMP_H, None,
     "eastl::string GetStateName(int index) const { return eastl::string(m_Animator.GetStateName(index)); }",
     "eastl::string GetStateName(int index) const { return eastl::string(m_Animator.GetStateName(0)); }"),

    # Unity 拼字与引擎拼字必须指向同一个槽位，否则两边悄悄分家。
    ("AM13 GetInteger reads the float slot instead",
     COMP_H, None,
     "int GetInteger(const eastl::string& name) const { return m_Animator.GetInt(name.c_str()); }",
     "int GetInteger(const eastl::string& name) const { return static_cast<int>(m_Animator.GetFloat(name.c_str())); }"),

    ("AM14 SetInteger is a no-op",
     COMP_H, None,
     "void SetInteger(const eastl::string& name, int value) { m_Animator.SetInt(name.c_str(), value); }",
     "void SetInteger(const eastl::string& name, int value) { (void)name; (void)value; }"),

    ("AM15 Update does not advance anything",
     COMP_H, None,
     "void Update(float delta_time) { m_Animator.Tick(delta_time); }",
     "void Update(float delta_time) { (void)delta_time; }"),

    ("AM16 GetFiredEventCount always reports 0",
     COMP_H, None,
     "int GetFiredEventCount() const { return static_cast<int>(m_Animator.GetFiredEvents().size()); }",
     "int GetFiredEventCount() const { return 0; }"),

    ("AM17 GetFiredEvent always returns empty",
     COMP_CPP, "eastl::string AnimationComponent::GetFiredEvent(int index) const",
     "    return events[static_cast<size_t>(index)];", "    return eastl::string();"),
]

LOCK = os.path.join(BAK, "zengine_mutate_anim.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_anim.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".animbak")
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
                with io.open(os.path.join(ROOT, ".mutate_anim.log"), "a", encoding="utf-8") as lg:
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
