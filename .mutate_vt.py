"""Mutation harness for the value-type facade slice (Vector2/Vector4/Matrix4x4).

Three families of mutants, because three layers of test exist and each layer
must be shown to actually bite:

* arithmetic wrong          -> VT*  (behaviour, C++ direct call)
* Unity's convention lost   -> VT5 / VT28 / VT31 / VT54b (the deliberate
  divergences: Vector2.right vs Vector3.right, 4-D vs 3-D length, OpenGL vs
  0..1 depth)
* the TS chain              -> Q1/Q2 (Mover.ts snapshot)

Wiring mutants (VTM34) exist so the source-level gates are themselves covered:
a gate that never fires is indistinguishable from no gate.

Run:  py .mutate_vt.py            (all)
      py .mutate_vt.py VTM01 VTM07  (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

SV_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.h")
SV_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
VM_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptVMSmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# ScriptVector.cpp 的函数都在 `namespace Runtime {` 里，缩进 4 空格，收尾是
# "\n    }"。.mutate_am.py 找的是 "\n}\n"（文件作用域），这里必须不一样。
END_OF_FUNC = "\n    }\n"


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    text = text.replace(old, new, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


def mutate_in(path, func_sig, old, new):
    """Replace only inside one function body.

    锚点必须限定在函数内：`LerpUnclamped(a, b, Clamp01(t))` 在 ScriptVector.cpp
    里有 **两份**（Vector2 和 Vector4），`if (sqr <= 0.0f)` 也有两份。全局
    replace(..., 1) 只会改到先出现的那个 —— 于是"Vector4 的变异"其实改的是
    Vector2，得到一个"什么也没验证"的 SURVIVED。
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    j = text.find(END_OF_FUNC, i)
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


# (name, path, [func_sig], old, new)   -- 5 元组：作用域限定到函数
# (name, path, old, new)               -- 4 元组：全局第一处
MUTATIONS = [
    # ---- Vector2 ----------------------------------------------------------
    ("VTM01 Vector2.right is -X after all", SV_CPP,
     "    ::Vector2 Vector2::GetRight()\n    {\n        return ::Vector2(1.0f, 0.0f);",
     "    ::Vector2 Vector2::GetRight()\n    {\n        return ::Vector2(-1.0f, 0.0f);"),

    ("VTM02 Vector2.Angle returns radians, not degrees", SV_CPP,
     "::Vector2 Vector2::Angle(",
     "        return Math::RadiansToDegrees(std::acos(ClampCos(a.x * b.x + a.y * b.y)));",
     "        return std::acos(ClampCos(a.x * b.x + a.y * b.y));"),

    ("VTM03 Vector2.SignedAngle has the cross product backwards", SV_CPP,
     "        const float cross = from.x * to.y - from.y * to.x;",
     "        const float cross = from.y * to.x - from.x * to.y;"),

    ("VTM04 Vector2.Lerp does not clamp t", SV_CPP,
     "::Vector2 Vector2::Lerp(",
     "        return LerpUnclamped(a, b, Clamp01(t));",
     "        return LerpUnclamped(a, b, t);"),

    ("VTM05 Vector2.Normalize drops the zero guard (NaN)", SV_CPP,
     "::Vector2 Vector2::Normalize(",
     "        if (sqr <= 0.0f)\n        {\n            return ::Vector2(0.0f, 0.0f);\n        }\n",
     ""),

    ("VTM06 Vector2.Perpendicular turns the other way", SV_CPP,
     "        return ::Vector2(-v.y, v.x);",
     "        return ::Vector2(v.y, -v.x);"),

    ("VTM07 Vector2.Reflect assumes the normal is already unit length", SV_CPP,
     "        const ::Vector2 n = Normalize(normal);",
     "        const ::Vector2 n = normal;"),

    ("VTM08 Vector2.ClampMagnitude always renormalizes", SV_CPP,
     "::Vector2 Vector2::ClampMagnitude(",
     "        if (sqr <= max_length * max_length)\n        {\n            return v;\n        }\n",
     ""),

    ("VTM09 Vector2.MoveTowards never snaps to the target", SV_CPP,
     "::Vector2 Vector2::MoveTowards(",
     "        if (sqr == 0.0f || (max_distance >= 0.0f && sqr <= max_distance * max_distance))\n"
     "        {\n            return b;\n        }\n",
     ""),

    ("VTM10 Vector2.Scale is a uniform scale by b.x", SV_CPP,
     "::Vector2 Vector2::Scale(",
     "        return ::Vector2(a.x * b.x, a.y * b.y);",
     "        return ::Vector2(a.x * b.x, a.y * b.x);"),

    ("VTM11 Vector2.Min returns its first argument", SV_CPP,
     "::Vector2 Vector2::Min(",
     "        return ::Vector2(std::min(a.x, b.x), std::min(a.y, b.y));",
     "        return a;"),

    ("VTM12 Vector2.kEpsilon is 1e-6", SV_CPP,
     "        return 0.00001f;",
     "        return 0.000001f;"),

    ("VTM13 Vector2.Dot forgets the y term", SV_CPP,
     "        return a.x * b.x + a.y * b.y;",
     "        return a.x * b.x;"),

    # ---- Vector4 ----------------------------------------------------------
    ("VTM14 Vector4.Magnitude is 3-D (drops w)", SV_CPP,
     "float Vector4::Magnitude(",
     "        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);",
     "        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);"),

    ("VTM15 Vector4.Normalize divides by the 3-D length", SV_CPP,
     "::Vector4 Vector4::Normalize(",
     "        const float sqr = SqrMagnitude(v);",
     "        const float sqr = v.x * v.x + v.y * v.y + v.z * v.z;"),

    ("VTM16 Vector4.Dot forgets w", SV_CPP,
     "        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;",
     "        return a.x * b.x + a.y * b.y + a.z * b.z;"),

    ("VTM17 Vector4.Lerp does not clamp t", SV_CPP,
     "::Vector4 Vector4::Lerp(",
     "        return LerpUnclamped(a, b, Clamp01(t));",
     "        return LerpUnclamped(a, b, t);"),

    ("VTM18 Vector4.Project drops the zero guard (NaN)", SV_CPP,
     "::Vector4 Vector4::Project(",
     "        if (bb <= 0.0f)\n        {\n            return ::Vector4(0.0f, 0.0f, 0.0f, 0.0f);\n        }\n",
     ""),

    ("VTM19 Vector4.Max returns its first argument", SV_CPP,
     "::Vector4 Vector4::Max(",
     "        return ::Vector4(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z),\n"
     "                         std::max(a.w, b.w));",
     "        return a;"),

    # ---- Matrix4x4 --------------------------------------------------------
    ("VTM20 Matrix4x4.Inverse drops the singular guard (NaN)", SV_CPP,
     "::Matrix4x4 Matrix4x4::Inverse(",
     "        if (std::fabs(m.determinant()) <= 1.0e-12f)\n        {\n"
     "            return ::Matrix4x4::ZERO;\n        }\n",
     ""),

    ("VTM21 Matrix4x4.Transpose is the identity function", SV_CPP,
     "        return m.transpose();",
     "        return m;"),

    ("VTM22 Matrix4x4.MultiplyPoint ignores the translation", SV_CPP,
     "::Vector3 Matrix4x4::MultiplyPoint(",
     "        return m.transformCoord(v);",
     "        return MultiplyVector(m, v);"),

    ("VTM23 Matrix4x4.MultiplyVector adds the translation", SV_CPP,
     "::Vector3 Matrix4x4::MultiplyVector(",
     "        return ::Vector3(m.m_Mat[0][0] * v.x + m.m_Mat[0][1] * v.y + m.m_Mat[0][2] * v.z,\n"
     "                         m.m_Mat[1][0] * v.x + m.m_Mat[1][1] * v.y + m.m_Mat[1][2] * v.z,\n"
     "                         m.m_Mat[2][0] * v.x + m.m_Mat[2][1] * v.y + m.m_Mat[2][2] * v.z);",
     "        return m.transformCoord(v);"),

    ("VTM24 Matrix4x4.TRS swaps position and scale", SV_CPP,
     "        m.MakeTransform(pos, s, q);",
     "        m.MakeTransform(s, pos, q);"),

    ("VTM25 Matrix4x4.Perspective treats fov as radians", SV_CPP,
     "        return Math::MakePerspectiveMatrix(Radian(Math::DegreesToRadians(fov)), aspect, zNear, zFar);",
     "        return Math::MakePerspectiveMatrix(Radian(fov), aspect, zNear, zFar);"),

    ("VTM26 Matrix4x4.Multiply has the operands backwards", SV_CPP,
     "        return a.concatenate(b);",
     "        return b.concatenate(a);"),

    ("VTM27 Matrix4x4.GetColumn returns a row", SV_CPP,
     "::Vector4 Matrix4x4::GetColumn(",
     "        return ::Vector4(m.m_Mat[0][i], m.m_Mat[1][i], m.m_Mat[2][i], m.m_Mat[3][i]);",
     "        return ::Vector4(m.m_Mat[i][0], m.m_Mat[i][1], m.m_Mat[i][2], m.m_Mat[i][3]);"),

    ("VTM28 Matrix4x4.IsIdentity always says yes", SV_CPP,
     "        return m == ::Matrix4x4::IDENTITY;",
     "        return true;"),

    ("VTM29 Matrix4x4.LookAt swaps eye and target", SV_CPP,
     "        return Math::MakeLookAtMatrix(from, to, up);",
     "        return Math::MakeLookAtMatrix(to, from, up);"),

    ("VTM30 Matrix4x4.Ortho uses the 0..1 depth variant", SV_CPP,
     "        return Math::MakeOrthographicProjectionMatrix(left, right, bottom, top, zNear, zFar);",
     "        return Math::MakeOrthographicProjectionMatrix01(left, right, bottom, top, zNear, zFar);"),

    ("VTM31 Matrix4x4.Rotate uses the transposed rotation", SV_CPP,
     "        m.setMatrix3x3(Matrix3x3(q));",
     "        m.setMatrix3x3(Matrix3x3(q).transpose());"),

    # ---- Vector3.Scale 的 Unity 语义 --------------------------------------
    ("VTM32 Vector3.Scale is a uniform scale again", SV_CPP,
     "::Vector3 Vector3::Scale(",
     "        return VectorMath::Mul(a, b);",
     "        return VectorMath::Scale(a, b.x);"),

    ("VTM33 Vector3.ScaleUniform ignores the scalar", SV_CPP,
     "::Vector3 Vector3::ScaleUniform(",
     "        return VectorMath::Scale(v, scalar);",
     "        return VectorMath::Scale(v, 1.0f);"),

    # ---- 接线门本身 --------------------------------------------------------
    ("VTM34 the Matrix4x4 facade loses its ZCLASS", SV_H,
     "    ZCLASS()\n    class Matrix4x4 : public Object",
     "    class Matrix4x4 : public Object"),
]

LOCK = os.path.join(BAK, "zengine_mutate_vt.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_vt.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            if len(entry) == 5:
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".vtbak")
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
