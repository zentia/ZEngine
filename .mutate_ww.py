"""Mutation harness for the WaitUntil / WaitWhile slice.

Every failure mode in this slice is the same shape: **the coroutine silently
hangs**, or **silently never waits at all**. Neither crashes, neither logs on
the happy path, and both look fine to a test that just advances frames until
something moves. So the mutants below are mostly "one link in a six-link chain
removed", and the tests that have to catch them are the ones that COUNT.

Two of the mutants (WW11, WW12) are pure leaks: a pesapi_value_ref that is
never released is invisible to the GC, so no runtime assertion can see it.
Those two are caught by the source-level counters BF44 / BF48, which is exactly
why those counters exist and why their thresholds are exact.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")
BEHAVIOUR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Behaviour.cpp")
TS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
YIELD_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Coroutine\ScriptYield.h")

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
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new):
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


TS_STEP = "bool TypeScriptComponent::StepScriptCoroutine(uint64_t script_id, ScriptYield& out)"
TS_EVAL = "bool TypeScriptComponent::EvalScriptPredicate(uint64_t predicate_id, bool& out_value)"
TS_SET = "void TypeScriptComponent::SetCoroutinePredicate(uint64_t script_id, void* predicate_ref)"
TS_END = "void TypeScriptComponent::EndScriptCoroutine(uint64_t script_id)"
TS_TEARDOWN = "void TypeScriptComponent::TearDown()"
ENV_PRED = "bool ScriptEnv::CallPredicate(pesapi_value_ref fn, bool& out_value)"


MUTATIONS = [
    # ---- shim：标记 --------------------------------------------------------
    # 少一个字母 => GeneratorNext 认不出 => 降级成"下一帧继续" => WaitUntil
    # 变成没等待（静默的语义降级，不是错误）。
    ("WW1 the WaitUntil tag is misspelled", ENV_CPP, None,
     "{ __zWait: 'until', fn: fn }",
     "{ __zWait: 'untill', fn: fn }"),

    ("WW2 WaitUntil and WaitWhile swap tags", ENV_CPP, None,
     "{ __zWait: 'until', fn: fn }",
     "{ __zWait: 'while', fn: fn }"),

    # ---- GeneratorNext：建强引用 -------------------------------------------
    # 不建 ref => out.predicate 为空 => 降级成 kNull。没有谓词，"等下去"就
    # 无从谈起，而日志只在 LOG_ERROR 里出现一次。
    ("WW3 GeneratorNext never keeps the predicate", ENV_CPP, None,
     "                    out.predicate = pesapi_create_value_ref(m_Papis, env, fn, 0);",
     "                    out.predicate = nullptr;"),

    # ---- CallPredicate -----------------------------------------------------
    ("WW4 the predicate always reports true", ENV_CPP, ENV_PRED,
     "        out_value = pesapi_get_value_bool(m_Papis, env, returned) != 0;",
     "        out_value = true;"),

    ("WW5 the predicate always reports false", ENV_CPP, ENV_PRED,
     "        out_value = pesapi_get_value_bool(m_Papis, env, returned) != 0;",
     "        out_value = false;"),

    ("WW6 the predicate is never actually called", ENV_CPP, ENV_PRED,
     "        pesapi_value returned = pesapi_call_function(m_Papis, env, callable, undefined_val, 0, nullptr);",
     "        pesapi_value returned = nullptr;"),

    # ---- pump 的转发 -------------------------------------------------------
    # 两个都转发成同一个：WaitWhile 于是永远立刻恢复（看起来"工作得很好"），
    # 或 WaitUntil 永远立刻恢复（K3 挂）。
    ("WW7 kWhile is forwarded to WaitUntil", BEHAVIOUR_CPP, None,
     "                case ScriptYieldKind::kWhile:\n"
     "                    co_yield WaitWhile {[self, script_id]() {",
     "                case ScriptYieldKind::kWhile:\n"
     "                    co_yield WaitUntil {[self, script_id]() {"),

    # ⚠️ 只锚 `co_yield` 那一行：`case` 标签和它之间隔着一段注释（第一版把
    # 注释一起写进 old 串，结果 PATTERN NOT FOUND）。
    ("WW8 kUntil is forwarded to WaitWhile", BEHAVIOUR_CPP, None,
     "                    co_yield WaitUntil {[self, script_id]() {",
     "                    co_yield WaitWhile {[self, script_id]() {"),

    # ---- TypeScriptComponent 的转手 / 轮询 ---------------------------------
    ("WW9 StepScriptCoroutine drops the predicate instead of storing it", TS_CPP, TS_STEP,
     "        SetCoroutinePredicate(script_id, out.predicate);",
     "        SetCoroutinePredicate(script_id, nullptr);"),

    ("WW10 EvalScriptPredicate never asks the script", TS_CPP, TS_EVAL,
     "    return sm->GetEnv()->CallPredicate(static_cast<pesapi_value_ref>(predicate), out_value);",
     "    return true;"),

    # 纯泄漏：运行期观测不到（GC 看不到强引用）。只有 BF44 / BF48 这两个
    # 数调用点的门能抓到 —— 这就是为什么它们的阈值是精确的。
    ("WW11 the old predicate is never released on replace", TS_CPP, TS_SET,
     "    if (slot != nullptr && slot != predicate_ref)\n"
     "    {\n"
     "        if (auto* sm = GET_SYSTEM(ScriptingManager))\n"
     "        {\n"
     "            sm->ReleaseCoroutineValue(static_cast<pesapi_value_ref>(slot));\n"
     "        }\n"
     "    }\n",
     ""),

    ("WW12 TearDown never releases the predicates", TS_CPP, TS_TEARDOWN,
     "    for (void* predicate : m_CoroutinePredicates)\n"
     "    {\n"
     "        if (predicate != nullptr && sm != nullptr)\n"
     "        {\n"
     "            sm->ReleaseCoroutineValue(static_cast<pesapi_value_ref>(predicate));\n"
     "        }\n"
     "    }\n"
     "    m_CoroutinePredicates.clear();",
     "    m_CoroutinePredicates.clear();"),

    ("WW13 EndScriptCoroutine leaves the predicate behind", TS_CPP, TS_END,
     "    SetCoroutinePredicate(script_id, nullptr);",
     ""),

    # ---- 声明 --------------------------------------------------------------
    ("WW14 the .d.ts stops declaring WaitUntil", BINDING_CPP, None,
     '        "declare class WaitUntil extends YieldInstruction {\\n"\n',
     ""),

    ("WW15 the .d.ts stops declaring WaitWhile", BINDING_CPP, None,
     '        "declare class WaitWhile extends YieldInstruction {\\n"\n',
     ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_ww.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_ww.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for name, path, func_sig, old, new in MUTATIONS:
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".wwbak")
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
