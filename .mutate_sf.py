"""Mutation harness for the static-field slice (Time / Screen / Mathf / Application).

The failure mode this slice can die from is *silent*: a ZSCRIPTPROPERTY that is
declared but never installed (or installed into pesapi's `Properties` instead of
`Variables`) does not crash and does not warn -- the property is simply
`undefined` in JS, and every C++ unit test stays green. So the checks that have
to bite here are the ones that evaluate real JS (ScriptVMSmokeTest D/J) plus the
source-level gates in UnityParitySmokeTest (BB).
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
TIME_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptTime\ScriptTime.h")
SCREEN_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptScreen\ScriptScreen.h")
MATH_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptMath\ScriptMath.h")
APP_H = os.path.join(ROOT, r"engine\Source\Runtime\Application\Application.h")
BIND_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
CODEGEN = os.path.join(ROOT, r"engine\tools\ZHeaderTool\zht\codegen.py")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
VM_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptVMSmokeTest.exe"
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
    # errors="replace": the test exes print Chinese through the console code
    # page, which is not always UTF-8 -- a raw `text=True` raises on decode and
    # takes the whole harness down in the middle of a mutation.
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, errors="replace", timeout=300)
    out = r.stdout or ""
    fails = [l for l in out.splitlines() if "FAIL" in l]
    tail = [l for l in out.splitlines() if ("checks," in l or "项检查" in l)]
    print("  %-24s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:4]:
        print("      " + l.strip())
    return r.returncode


def build():
    # LNK1236 / LNK1000 / LNK1106 on ZRuntime.lib are flaky here; retry.
    p = None
    for _ in range(3):
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest",
                            "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                           env=ENV, capture_output=True, text=True)
        if p.returncode != 0:
            continue
        p = subprocess.run(["cmake", "--build", ".", "--target", "ScriptVMSmokeTest",
                            "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                           env=ENV, capture_output=True, text=True)
        if p.returncode == 0:
            break
    return p


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines()[-3:]:
            print("    " + line)
        return "NOT CAUGHT"
    rc = run(PARITY_EXE) | run(VM_EXE)
    if rc == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("SF1 Time.deltaTime is not declared at all", TIME_H,
     '    ZSCRIPTPROPERTY(Name = "deltaTime", Getter = "GetDeltaTime")',
     ''),

    ("SF2 Screen.width is published under the wrong (Unity-wrong) name", SCREEN_H,
     'ZSCRIPTPROPERTY(Name = "width", Getter = "GetWidth")',
     'ZSCRIPTPROPERTY(Name = "Width", Getter = "GetWidth")'),

    ("SF3 Mathf.PI is spelled Mathf.pi", MATH_H,
     'ZSCRIPTPROPERTY(Name = "PI", Getter = "Pi")',
     'ZSCRIPTPROPERTY(Name = "pi", Getter = "Pi")'),

    ("SF4 Application.dataPath is declared but never published", APP_H,
     'ZSCRIPTPROPERTY(Name = "dataPath", Getter = "GetDataPath")',
     ''),

    ("SF5 timeScale is published read-only (no setter)", TIME_H,
     'ZSCRIPTPROPERTY(Name = "timeScale", Getter = "GetTimeScale", Setter = "SetTimeScale")',
     'ZSCRIPTPROPERTY(Name = "timeScale", Getter = "GetTimeScale")'),

    ("SF6 static properties are filed as instance ones (pesapi Properties)", BIND_CPP,
     "prop.isStatic ? 1 : 0,",
     "0,"),

    ("SF7 the Variables table is sized 0", BIND_CPP,
     "instance_prop_count, static_prop_count);",
     "instance_prop_count, 0);"),

    ("SF8 codegen stops stamping isStatic", CODEGEN,
     '"true" if sp.is_static else "false"))',
     '"false"))'),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and name.split()[0] != only:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".sfbak")
        shutil.copy2(path, bak)
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
        print("%-70s %s" % (n, r))


if __name__ == "__main__":
    main()
