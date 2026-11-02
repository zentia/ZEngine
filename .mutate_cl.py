"""Mutation harness for the Color slice (AZ/BA checks + ScriptVMSmokeTest I).

Two exes, because this slice has two halves that fail differently:

  * UnityParitySmokeTest  -- the C++ implementation (Lerp clamping, HSV maths).
  * ScriptVMSmokeTest     -- the *binding*: `Color.red` reachable from real JS.
    A static property that is declared but never installed looks perfect to the
    first exe (it never touches pesapi) and shows up as `undefined` only here.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
COL_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptColor\ScriptColor.h")
COL_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptColor\ScriptColor.cpp")
BIND_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.h")
BIND_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
CODEGEN = os.path.join(ROOT, r"engine\tools\ZHeaderTool\zht\codegen.py")
PARSER = os.path.join(ROOT, r"engine\tools\ZHeaderTool\zht\parser.py")

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


def build():
    # LNK1236 / LNK1000 / LNK1106 on ZRuntime.lib are *flaky* here (882 GB free,
    # so it is not the disk): a bare retry passes. Retry so a mutation is never
    # written off as "caught by the linker" when the linker just hiccuped.
    p = None
    for attempt in range(3):
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest",
                            "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                           env=ENV, capture_output=True, text=True)
        if p.returncode != 0:
            print("    (retrying after a failed build: %s)" %
                  (", ".join(sorted(set(__import__("re").findall(r"LNK\d+", p.stdout)))) or "no LNK"))
            continue
        p = subprocess.run(["cmake", "--build", ".", "--target", "ScriptVMSmokeTest",
                            "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                           env=ENV, capture_output=True, text=True)
        if p.returncode == 0:
            break
        print("    (retrying after a failed build: %s)" %
              (", ".join(sorted(set(__import__("re").findall(r"LNK\d+", p.stdout)))) or "no LNK"))
    return p


def run(exe):
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, timeout=300)
    fails = [l for l in r.stdout.splitlines() if "FAIL" in l]
    tail = [l for l in r.stdout.splitlines() if ("checks," in l or "项检查" in l)]
    print("  %-24s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:6]:
        print("      " + l.strip())
    return r.returncode


def build_and_run():
    p = build()
    if p.returncode != 0:
        # LNK1179 is an expected outcome for C19 (static accessors back in the
        # method table): the object file itself is rejected.
        is_link_error = ("LNK1179" in p.stdout or "LNK1236" in p.stdout or
                         "LNK1000" in p.stdout or "LNK1314" in p.stdout)
        print("  BUILD FAILED rc=%d%s" % (p.returncode, " (link)" if is_link_error else ""))
        for line in p.stdout.splitlines()[-4:]:
            print("    " + line)
        return "build-failed" if is_link_error else "NOT CAUGHT"
    rc = run(PARITY_EXE) | run(VM_EXE)
    if rc == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("C1 Color.clear is opaque", COL_CPP,
     "return Make(0.0f, 0.0f, 0.0f, 0.0f);",
     "return Make(0.0f, 0.0f, 0.0f, 1.0f);"),

    ("C2 Color.black is transparent", COL_CPP,
     "return Make(0.0f, 0.0f, 0.0f, 1.0f);\n    }\n\n    ::Color Color::GetRed()",
     "return Make(0.0f, 0.0f, 0.0f, 0.0f);\n    }\n\n    ::Color Color::GetRed()"),

    ("C3 Color.yellow becomes pure (1,1,0)", COL_CPP,
     "return Make(1.0f, 235.0f / 255.0f, 4.0f / 255.0f, 1.0f);",
     "return Make(1.0f, 1.0f, 0.0f, 1.0f);"),

    ("C4 grey drifts off 50%", COL_CPP,
     "::Color Color::GetGrey()\n    {\n        return Make(0.5f, 0.5f, 0.5f, 1.0f);",
     "::Color Color::GetGrey()\n    {\n        return Make(0.4f, 0.5f, 0.5f, 1.0f);"),

    ("C5 Lerp stops clamping t", COL_CPP,
     "        if (clamped > 1.0f)\n            clamped = 1.0f;\n",
     ""),

    ("C6 LerpUnclamped clamps after all", COL_CPP,
     "        out.r = a.r + (b.r - a.r) * t;",
     "        t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);\n        out.r = a.r + (b.r - a.r) * t;"),

    ("C7 Lerp leaves alpha at the start value", COL_CPP,
     "        out.a = a.a + (b.a - a.a) * t;",
     "        out.a = a.a;"),

    ("C8 HSVToRGB: negative saturation no longer degrades to grey", COL_CPP,
     "        if (s <= 0.0f)",
     "        if (s <= -1.0f)"),

    ("C9 HSVToRGB: two sectors swapped", COL_CPP,
     "            case 0:\n                return Make(v, t, p, 1.0f);\n"
     "            case 1:\n                return Make(q, v, p, 1.0f);",
     "            case 0:\n                return Make(q, v, p, 1.0f);\n"
     "            case 1:\n                return Make(v, t, p, 1.0f);"),

    ("C10 HSVToRGB: hue no longer wraps", COL_CPP,
     "        float hue = h - std::floor(h);",
     "        float hue = h;"),

    ("C11 RGBToHSV: the V == 0 branch is dropped", COL_CPP,
     "        if (v != 0.0f)\n",
     "        if (true)\n"),

    ("C12 RGBToHSV: blue gets the green offset", COL_CPP,
     "            offset = 4.0f;",
     "            offset = 2.0f;"),

    ("C13 RGBToHSV: saturation is not normalised by V", COL_CPP,
     "                s = diff / v;",
     "                s = diff;"),

    ("C14 Converter<Color>: missing alpha defaults to 0", BIND_H,
     "                        : 1.0f;",
     "                        : 0.0f;"),

    ("C15 Converter<Color>: green and blue swapped", BIND_H,
     '            out.g = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "g")));\n'
     '            out.b = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "b")));',
     '            out.g = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "b")));\n'
     '            out.b = static_cast<float>(apis->get_value_double(env, apis->get_property(env, v, "g")));'),

    ("C22 Converter<Color>::ToJs: green and blue swapped", BIND_H,
     '            apis->set_property(env, o, "g", apis->create_double(env, v.g));\n'
     '            apis->set_property(env, o, "b", apis->create_double(env, v.b));',
     '            apis->set_property(env, o, "g", apis->create_double(env, v.b));\n'
     '            apis->set_property(env, o, "b", apis->create_double(env, v.g));'),

    ("C23 Converter<Color>: an explicit alpha is ignored", BIND_H,
     "            out.a = (alpha != nullptr && apis->is_undefined(env, alpha) == 0)\n"
     "                        ? static_cast<float>(apis->get_value_double(env, alpha))\n"
     "                        : 1.0f;",
     "            out.a = 1.0f;"),

    ("C16 the Variables table is sized 0 (static properties never installed)", BIND_CPP,
     "                                         instance_prop_count, static_prop_count);",
     "                                         instance_prop_count, 0);"),

    ("C17 static properties are filed as instance ones", BIND_CPP,
     "prop.isStatic ? 1 : 0,",
     "0,"),

    ("C18 the .d.ts drops the `static` prefix", BIND_CPP,
     '                if (prop.isStatic)\n                    out += "static ";',
     '                if (false)\n                    out += "static ";'),

    ("C19 codegen stops stamping isStatic", CODEGEN,
     '                      "true" if sp.is_static else "false"))',
     '                      "false"))'),

    ("C20 static accessors go back into the method table (LNK1179)", CODEGEN,
     "    methods = [m for m in cls.methods if not (m.is_static and m.name in static_accessors)]",
     "    methods = list(cls.methods)"),

    ("C21 Color is no longer a marshallable ZPROPERTY type", PARSER,
     '    "Color": "Color",',
     '    "Color": "never",'),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and name.split()[0] != only:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".clbak")
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
