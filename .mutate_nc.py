"""Mutation harness for the nested-coroutine slice (`yield return StartCoroutine(other)`).

Every failure mode here has the SAME shape: **the nested wait silently degrades
into a one-frame wait**. No crash, no log, and the outer coroutine still reaches
its last step -- just one frame early. Only N5 (the outer must still be parked
while the inner is mid-flight) can see it, which is why the inner routine in
Mover.ts has to yield more than once.

Two mutants (NC11, NC12) are type-level only: no C++ test runs tsc, so the
`.d.ts` shape is guarded purely by the source scan BF55.

Note on equivalence: `id == 0` degrading to `kNull` versus being passed through
as `WaitForCoroutine{0}` are behaviourally identical (0 is kInvalidId and the
scheduler never hands it out, so IsRunning(0) is false == WaitForNextFrame).
NC4 therefore catches "the id is lost", not "the guard is missing" -- the guard
itself is defensive and has no observable mutant.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

ENV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Wrapper\ScriptEnv.cpp")
BEHAVIOUR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Behaviour.cpp")
COROUTINE_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Coroutine\Coroutine.cpp")
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


NC_BRANCH = (
    "        pesapi_value id_val = pesapi_get_property(m_Papis, env, yielded, \"__zId\");\n"
    "        if (id_val && zIsNumber(m_Papis, env, id_val))\n"
    "        {\n"
)

MUTATIONS = [
    # ---- shim：句柄上的 id ---------------------------------------------------
    # id 写成 0 => GeneratorNext 的 id>0 判据落空 => 降级成 kNull => 外层只晚一帧。
    ("NC1 the handle carries id 0", ENV_CPP,
     "{ this.__zId = id; this.__zMethod = method; }",
     "{ this.__zId = 0; this.__zMethod = method; }"),

    # id 写成字符串 => zIsNumber 为假 => 落到 __zWait 分支 => 未知对象 => kNull。
    ("NC2 the handle carries the id as a string", ENV_CPP,
     "{ this.__zId = id; this.__zMethod = method; }",
     "{ this.__zId = '' + id; this.__zMethod = method; }"),

    # shim 干脆不交句柄：脚本侧 `yield null`，一切都还"能跑"。
    ("NC3 StartCoroutine hands back null instead of a handle", ENV_CPP,
     "    return id ? new g.Coroutine(id, name) : null;",
     "    return null;"),

    # ---- GeneratorNext：按形状认 --------------------------------------------
    # 整段删掉：句柄上 __zWait 是 undefined => "未知对象" => kNull。
    ("NC4 GeneratorNext has no __zId branch at all", ENV_CPP, NC_BRANCH, ""),

    # 只认到形状、不把 id 带出来：WaitForCoroutine{0} 与"下一帧"等价。
    ("NC5 the id is recognised but not carried", ENV_CPP,
     "                out.coroutine_id = static_cast<uint64_t>(id);",
     "                out.coroutine_id = 0;"),

    # 形状认出来了，kind 却写成 kNull。
    ("NC6 the __zId branch sets the wrong kind", ENV_CPP,
     "                out.kind = ScriptYieldKind::kCoroutine;",
     "                out.kind = ScriptYieldKind::kNull;"),

    ("NC7 the __zId property name is misspelled", ENV_CPP,
     "pesapi_get_property(m_Papis, env, yielded, \"__zId\")",
     "pesapi_get_property(m_Papis, env, yielded, \"__zID\")"),

    # ---- pump 的转发 ---------------------------------------------------------
    ("NC8 the pump forwards kCoroutine to WaitForNextFrame", BEHAVIOUR_CPP,
     "                    co_yield WaitForCoroutine {yielded.coroutine_id};",
     "                    co_yield WaitForNextFrame {};"),

    ("NC9 the pump forwards a hardcoded id", BEHAVIOUR_CPP,
     "                    co_yield WaitForCoroutine {yielded.coroutine_id};",
     "                    co_yield WaitForCoroutine {0};"),

    # ---- 调度器 ---------------------------------------------------------------
    # 不判 IsRunning => 嵌套等待变成"下一帧"。
    ("NC10 the scheduler never blocks on a nested wait", COROUTINE_CPP,
     "            entry.blocked = IsRunning(nested->id);",
     "            entry.blocked = false;"),

    # 反过来：永远挡住 => 外层永远不恢复（Unity 里这是"协程卡死"）。
    ("NC11 the scheduler blocks forever on a nested wait", COROUTINE_CPP,
     "            entry.blocked = IsRunning(nested->id);",
     "            entry.blocked = true;"),

    # ---- .d.ts ----------------------------------------------------------------
    # 纯类型层：没有任何 C++ 测试跑 tsc，只有 BF55 这个源码扫描门能抓到。
    ("NC12 Coroutine no longer extends YieldInstruction", BINDING_CPP,
     '        "declare class Coroutine extends YieldInstruction {\\n"\n',
     '        "declare class Coroutine {\\n"\n'),

    ("NC13 the .d.ts stops declaring Coroutine", BINDING_CPP,
     '        "declare class Coroutine extends YieldInstruction {\\n"\n'
     '        "    private __zId: number;\\n"\n'
     '        "    private __zMethod: string;\\n"\n'
     '        "}\\n"\n',
     ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_nc.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_nc.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for name, path, old, new in MUTATIONS:
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".ncbak")
            subprocess.run(["git", "show", "HEAD:" + os.path.relpath(path, ROOT).replace("\\", "/")],
                           cwd=ROOT, env=ENV, stdout=io.open(bak, "wb"), stderr=subprocess.DEVNULL)
            try:
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
            print("%-58s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
