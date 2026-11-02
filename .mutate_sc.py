"""Mutation harness for the script-coroutine slice (StartCoroutine / yield).

The failure mode here is *silent*, twice over:

  1. The C++ adapter can be right while the JS shim is wrong. A typo in a
     `__zWait` tag, a missing branch in ScriptEnv::GeneratorNext, or a shim that
     never forwards to `component` -- all of them leave every C++-only test
     green, because CoroutineProbe answers the hooks itself and never touches
     JS. Only ScriptComponentSmokeTest's H group sees it.
  2. A coroutine that resumes in the *wrong phase* still produces a value, just
     one phase stale. Nothing throws. So the mutants below deliberately break
     the phase mapping and the timing, which is where "it works" and "it
     appears to work" come apart.

Every mutant must be caught by at least one of the three exes.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

BEHAVIOUR_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Behaviour.h")
BEHAVIOUR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Behaviour.cpp")
TS_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.h")
TS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Script\TypeScriptComponent.cpp")
ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")
BIND_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
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
    # LNK1236 / LNK1000 / LNK1106 on ZRuntime.lib are flaky here; retry.
    for _ in range(3):
        ok = True
        for target in ("UnityParitySmokeTest", "ScriptComponentSmokeTest"):
            p = subprocess.run(["cmake", "--build", ".", "--target", target,
                                "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                               env=ENV, capture_output=True, text=True)
            if p.returncode != 0:
                ok = False
                last = p
                break
            last = p
        if ok:
            return last
    return last


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in (p.stdout or "").splitlines()[-3:]:
            print("    " + line)
        return "NOT CAUGHT"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("SC1 the pump resumes WaitForEndOfFrame in the Update phase", BEHAVIOUR_CPP,
     "                    co_yield WaitForEndOfFrame {};",
     "                    co_yield WaitForNextFrame {};"),

    ("SC2 the pump resumes WaitForFixedUpdate in the Update phase", BEHAVIOUR_CPP,
     "                    co_yield WaitForFixedUpdate {};",
     "                    co_yield WaitForNextFrame {};"),

    ("SC3 the pump drops the WaitForSeconds duration", BEHAVIOUR_CPP,
     "                    co_yield WaitForSeconds {yielded.seconds};",
     "                    co_yield WaitForSeconds {0.0f};"),

    ("SC4 the pump uses wall-clock time for WaitForSeconds", BEHAVIOUR_CPP,
     "                    co_yield WaitForSeconds {yielded.seconds};",
     "                    co_yield WaitForSecondsRealtime {yielded.seconds};"),

    ("SC5 the pump never releases the generator on its own exit", BEHAVIOUR_CPP,
     "            if (yielded.done)\n            {\n                self->ReleaseScriptCoroutine(script_id);\n                co_return;",
     "            if (yielded.done)\n            {\n                co_return;"),

    ("SC6 a broken generator is polled forever instead of dropped", BEHAVIOUR_CPP,
     "                self->ReleaseScriptCoroutine(script_id);\n                co_return;\n            }\n            if (yielded.done)",
     "                co_return;\n            }\n            if (yielded.done)"),

    ("SC7 the destructor forgets to stop the coroutines", BEHAVIOUR_CPP,
     "    CancelAllInvokes();\n    StopAllCoroutines();",
     "    CancelAllInvokes();"),

    ("SC8 release is no longer exactly-once (no ownership flag)", BEHAVIOUR_CPP,
     "    if (!entry.generator_owned)\n    {\n        return;\n    }\n    entry.generator_owned = false;",
     "    if (false)\n    {\n        return;\n    }"),

    ("SC9 StartCoroutine does not reject a Behaviour with no script side", BEHAVIOUR_CPP,
     "    if (!StartScriptCoroutine(methodName.c_str(), script_id))",
     "    if (false && !StartScriptCoroutine(methodName.c_str(), script_id))"),

    ("SC10 StopCoroutine forgets to tell the scheduler", BEHAVIOUR_CPP,
     "        if (scheduler != nullptr)\n        {\n            scheduler->Stop(pending.id);\n        }\n        ReleasePendingGenerator(pending);",
     "        ReleasePendingGenerator(pending);"),

    ("SC11 WaitForEndOfFrame's shim tag is misspelled", ENV_CPP,
     "\"  g.WaitForEndOfFrame = function() { return { __zWait: 'endOfFrame' }; };\"",
     "\"  g.WaitForEndOfFrame = function() { return { __zWait: 'endOfFram' }; };\"",),

    ("SC12 WaitForSeconds' shim tag says realtime", ENV_CPP,
     "\"  g.WaitForSeconds = function(s) { return { __zWait: 'seconds', seconds: s }; };\"",
     "\"  g.WaitForSeconds = function(s) { return { __zWait: 'secondsRealtime', seconds: s }; };\""),

    ("SC13 GeneratorNext never reports done", ENV_CPP,
     "        out.done = pesapi_get_value_bool(m_Papis, env, done_val) != 0;",
     "        out.done = false;"),

    ("SC14 GeneratorNext drops the seconds payload", ENV_CPP,
     "            const double      secs = zGetNumber(\n                m_Papis, env, pesapi_get_property(m_Papis, env, yielded, \"seconds\"));",
     "            const double      secs = 0.0;"),

    ("SC15 the JS prototype never forwards StartCoroutine", ENV_CPP,
     "\"    var id = this.component.StartCoroutine(name);\"",
     "\"    var id = 0;\""),

    ("SC16 the JS prototype never forwards StopAllCoroutines", ENV_CPP,
     "        \"    if (this.component) this.component.StopAllCoroutines();\"",
     "        \"    ;\""),

    ("SC17 the .d.ts stops declaring StartCoroutine on Behaviour", BIND_CPP,
     "\"    StartCoroutine(methodName: string): Coroutine | null;\",",
     ""),

    ("SC18 script_id 0 is handed out (looks like 'no script side')", TS_CPP,
     "    out_script_id = static_cast<uint64_t>(m_CoroutineGenerators.size());",
     "    out_script_id = static_cast<uint64_t>(m_CoroutineGenerators.size()) - 1;"),

    # EQUIVALENT, and worth recording as such: `slot = nullptr` cannot be
    # observed while PendingCoroutine::generator_owned is in force, because the
    # only two readers of the slot are (a) StepScriptCoroutine, which is
    # unreachable once the scheduler entry is gone, and (b) EndScriptCoroutine,
    # which the flag already gates. Not a test gap -- a redundant defensive
    # line. (Run with `--sc19` to confirm; it reports NOT CAUGHT by design.)
    ("SC19 [EQUIVALENT] EndScriptCoroutine keeps the stale pointer in the slot", TS_CPP,
     "    slot = nullptr;",
     "    ;"),

    ("SC20 StepScriptCoroutine never actually steps the generator", TS_CPP,
     "    return sm->GeneratorNext(static_cast<pesapi_value_ref>(generator), out);",
     "    out = ScriptYield {};\n    return true;"),
]

# Mutants that are equivalent by construction and therefore expected to
# survive. Anything else surviving is a real gap.
EQUIVALENT = {"SC19"}


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and name.split()[0] != only:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".scbak")
        # Copy from a pristine source, never from a possibly-mutated one: an
        # interrupted harness leaves dirty backups and the NEXT run then
        # "restores" the mutation.
        subprocess.run(["git", "show", "HEAD:" + os.path.relpath(path, ROOT).replace("\\", "/")],
                       cwd=ROOT, env=ENV, stdout=io.open(bak, "wb"), stderr=subprocess.DEVNULL)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
            os.utime(path, None)
            touched.add(path)
        print()
    for path in touched:
        os.utime(path, None)
    print("---- summary ----")
    for n, r in results:
        tag = n.split()[0]
        verdict = ("equivalent (expected)" if tag in EQUIVALENT else "NOT CAUGHT") if r == "NOT CAUGHT" else r
        print("%-64s %s" % (n, verdict))


if __name__ == "__main__":
    main()
