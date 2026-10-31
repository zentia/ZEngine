"""Mutation harness for the Bounds + Color32 slice.

What this slice is made of, and therefore what can rot silently:

  * Pure arithmetic with **no** observable side effect other than the number it
    returns. `Expand` forgetting Unity's `amount *= .5f` still produces a
    perfectly self-consistent box (min/max/size all agree) -- it is just twice
    as big as the caller asked for. Only an assertion on the actual number
    catches it.
  * Boundary conventions that are invisible on symmetric samples: `Contains`
    including the boundary, `Intersects` counting a touching box. A fixture
    built out of zeros or of equal components cannot tell them apart.
  * Two conversions (`Color32 <-> Color`) whose *rounding* is the whole
    contract, and a NaN-producing division that a ray test would otherwise
    turn into a false hit.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

BOUNDS_H = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\Bounds.h")
BOUNDS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\Bounds.cpp")
COLOR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Core\Color\Color.cpp")
BINDING_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.h")
FACADE_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptBounds\ScriptBounds.cpp")

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
    ("BD1 GetSize returns extents instead of 2 x extents", BOUNDS_H,
     "    Vector3 GetSize() const { return extents * 2.0f; }",
     "    Vector3 GetSize() const { return extents; }"),

    ("BD2 SetSize forgets to halve", BOUNDS_H,
     "    void SetSize(const Vector3& size) { extents = size * 0.5f; }",
     "    void SetSize(const Vector3& size) { extents = size; }"),

    ("BD3 SetMinMax keeps min as the centre", BOUNDS_H,
     "        center = (min + max) * 0.5f;\n        extents = (max - min) * 0.5f;",
     "        center = min;\n        extents = (max - min) * 0.5f;"),

    ("BD4 Contains excludes the boundary", BOUNDS_H,
     "        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&\n"
     "               point.z >= min.z && point.z <= max.z;",
     "        return point.x > min.x && point.x < max.x && point.y > min.y && point.y < max.y &&\n"
     "               point.z > min.z && point.z < max.z;"),

    ("BD5 Intersects does not count touching boxes", BOUNDS_H,
     "        return min.x <= other_max.x && max.x >= other_min.x && min.y <= other_max.y && max.y >= other_min.y &&\n"
     "               min.z <= other_max.z && max.z >= other_min.z;",
     "        return min.x < other_max.x && max.x > other_min.x && min.y < other_max.y && max.y > other_min.y &&\n"
     "               min.z < other_max.z && max.z > other_min.z;"),

    ("BD6 ClosestPoint never clamps", BOUNDS_H,
     "        if (out.x < min.x)\n            out.x = min.x;",
     "        if (false)\n            out.x = min.x;"),

    # 用"曼哈顿距离"当替身而不是 sqrt：sqrt 要 <cmath>，而加不加这个 include
    # 会让变异体变成"编译不过"——那是无效变异，不是被抓住。曼哈顿在 (7,0,0)
    # 上是 7，正确值是 49，同样能分辨"忘了平方"。
    ("BD7 SqrDistance is not squared", BOUNDS_H,
     "        return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;",
     "        return delta.x + delta.y + delta.z;"),

    ("BD8 Expand(float) forgets Unity's amount *= .5f", BOUNDS_H,
     "        amount *= 0.5f;\n        extents = extents + Vector3(amount, amount, amount);",
     "        extents = extents + Vector3(amount, amount, amount);"),

    ("BD9 Expand(Vector3) forgets the halving", BOUNDS_H,
     "        extents = extents + amount * 0.5f;",
     "        extents = extents + amount;"),

    ("BD10 Encapsulate(point) only grows max, never min", BOUNDS_H,
     "        min.makeFloor(point);\n        max.makeCeil(point);",
     "        max.makeCeil(point);"),

    ("BD11 Encapsulate(Bounds) only takes the other centre", BOUNDS_H,
     "        Encapsulate(other.GetMin());\n        Encapsulate(other.GetMax());",
     "        Encapsulate(other.center);"),

    ("BD12 IntersectRay drops the parallel-axis guard (divides by zero)", BOUNDS_CPP,
     "        if (dir[axis] > -1e-8f && dir[axis] < 1e-8f)",
     "        if (false)"),

    ("BD13 IntersectRay reports a miss when the origin is inside", BOUNDS_CPP,
     "    if (t_exit < 0.0f)\n    {\n        // 整个盒都在射线起点背后。\n        return false;\n    }",
     "    if (t_exit < 0.0f || t_enter < 0.0f)\n    {\n        return false;\n    }"),

    ("BD14 IntersectRay returns the negative entry distance", BOUNDS_CPP,
     "    out_distance = (t_enter >= 0.0f) ? t_enter : 0.0f;",
     "    out_distance = t_enter;"),

    ("BD15 the byte conversion rounds instead of truncating", COLOR_CPP,
     "        return static_cast<uint8_t>(Clamp01(channel_0_1) * 255.0f);",
     "        return static_cast<uint8_t>(Clamp01(channel_0_1) * 255.0f + 0.5f);"),

    ("BD16 the byte conversion stops clamping the channel", COLOR_CPP,
     "        return static_cast<uint8_t>(Clamp01(channel_0_1) * 255.0f);",
     "        return static_cast<uint8_t>(channel_0_1 * 255.0f);"),

    ("BD17 Color32 -> Color divides by 256", COLOR_CPP,
     "    out.r = static_cast<float>(r) / 255.0f;",
     "    out.r = static_cast<float>(r) / 256.0f;"),

    ("BD18 Color32::Lerp stops clamping t", COLOR_CPP,
     "    return LerpChannels(from, to, Clamp01(t));",
     "    return LerpChannels(from, to, t);"),

    ("BD19 the Color32 converter defaults a missing alpha to transparent", BINDING_H,
     "            out.a = channel(\"a\", 255);",
     "            out.a = channel(\"a\", 0);"),

    ("BD20 the Bounds converter drops extents on the way in", BINDING_H,
     "            if (extents != nullptr && apis->is_undefined(env, extents) == 0)\n"
     "                out.extents = Converter<Vector3>::FromJs(apis, env, extents);",
     "            (void)extents;"),

    ("BD21 Bounds.Create treats its second argument as extents", FACADE_CPP,
     "        return ::Bounds::FromCenterSize(center, size);",
     "        return ::Bounds(center, size);"),
]

# Nothing is pre-registered as equivalent: every mutant above changes a number
# that at least one assertion observes, or (BD12) turns a NaN comparison into a
# false hit. If one survives, that is a real gap, not a formality.
EQUIVALENT = set()

LOCK = os.path.join(BAK, "zengine_mutate_bc.lock")


def main():
    # 两个 harness 同时跑会互相踩 MSBuild（MSB3371 + LNK1104），还都往同一个
    # log 写。用锁文件挡住第二个实例。
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_bc.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    try:
        for name, path, old, new in MUTATIONS:
            if only and name.split()[0] != only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".bcbak")
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
            tag = n.split()[0]
            verdict = ("equivalent (expected)" if tag in EQUIVALENT else "NOT CAUGHT") if r == "NOT CAUGHT" else r
            print("%-64s %s" % (n, verdict))
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
