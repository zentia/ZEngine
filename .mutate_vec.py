"""Mutation harness for the Vector3 / Quaternion facade slice (BC / K checks).

The two ways this slice can rot:

  * the axis constants get "corrected" to Unity's numbers (Y-up) -- wrong for a
    Z-up engine, and invisible to any C++ test that only checks "is it a unit
    vector";
  * the facade stops forwarding to VectorMath and quietly grows its own copy
    (then Vector3.Lerp and VectorMath.Lerp disagree about clamping).

(Not to be confused with .mutate_vq.py, which covers the older
VectorMath / QuaternionMath slice.)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
VEC_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.h")
VEC_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.cpp")

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
    p = None
    for _ in range(3):
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest",
                            "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                           env=ENV, capture_output=True, text=True, errors="replace")
        if p.returncode != 0:
            continue
        p = subprocess.run(["cmake", "--build", ".", "--target", "ScriptVMSmokeTest",
                            "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                           env=ENV, capture_output=True, text=True, errors="replace")
        if p.returncode == 0:
            break
    return p


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in (p.stdout or "").splitlines()[-3:]:
            print("    " + line)
        return "NOT CAUGHT"
    rc = run(PARITY_EXE) | run(VM_EXE)
    if rc == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("V1 up is +Y (Unity's numbers copied into a Z-up engine)", VEC_CPP,
     "    ::Vector3 Vector3::GetUp()\n    {\n        return Make(0.0f, 0.0f, 1.0f);",
     "    ::Vector3 Vector3::GetUp()\n    {\n        return Make(0.0f, 1.0f, 0.0f);"),

    ("V2 forward is +Y instead of -Y", VEC_CPP,
     "    ::Vector3 Vector3::GetForward()\n    {\n        return Make(0.0f, -1.0f, 0.0f);",
     "    ::Vector3 Vector3::GetForward()\n    {\n        return Make(0.0f, 1.0f, 0.0f);"),

    ("V3 left is no longer the opposite of right", VEC_CPP,
     "    ::Vector3 Vector3::GetLeft()\n    {\n        return Make(-1.0f, 0.0f, 0.0f);",
     "    ::Vector3 Vector3::GetLeft()\n    {\n        return Make(1.0f, 0.0f, 0.0f);"),

    ("V4 Vector3.Lerp no longer clamps t", VEC_CPP,
     "        if (clamped > 1.0f)\n        {\n            clamped = 1.0f;\n        }\n"
     "        return LerpUnclamped(a, b, clamped);",
     "        return LerpUnclamped(a, b, clamped);"),

    ("V5 Vector3.zero is really one", VEC_CPP,
     "    ::Vector3 Vector3::GetZero()\n    {\n        return Make(0.0f, 0.0f, 0.0f);",
     "    ::Vector3 Vector3::GetZero()\n    {\n        return Make(1.0f, 1.0f, 1.0f);"),

    ("V6 Quaternion.identity is the zero quaternion", VEC_CPP,
     "        return ::Quaternion::IDENTITY;",
     "        return ::Quaternion::ZERO;"),

    ("V7 the zero property is not published at all", VEC_H,
     '        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")',
     ''),

    ("V8 the identity property is not published at all", VEC_H,
     '        ZSCRIPTPROPERTY(Name = "identity", Getter = "GetIdentity")',
     ''),

    # Unity's numbers for right/left. Every "is it a unit vector" assertion
    # stays green; only BD3 (Vector3.right vs transform.right) can see it.
    ("V9 right is +X (Unity's numbers): contradicts transform.right", VEC_CPP,
     "    ::Vector3 Vector3::GetRight()\n    {\n        return Make(-1.0f, 0.0f, 0.0f);",
     "    ::Vector3 Vector3::GetRight()\n    {\n        return Make(1.0f, 0.0f, 0.0f);"),

    ("V10 right and left collapse onto the same axis", VEC_CPP,
     "    ::Vector3 Vector3::GetLeft()\n    {\n        return Make(1.0f, 0.0f, 0.0f);",
     "    ::Vector3 Vector3::GetLeft()\n    {\n        return Make(-1.0f, 0.0f, 0.0f);"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and name.split()[0] != only:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".vecbak")
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
