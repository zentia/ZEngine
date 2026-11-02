"""Mutation harness for the VectorMath/QuaternionMath slice (AK/AL checks)."""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_MATH = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptMath\ScriptMath.h")
EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    text = io.open(path, encoding="utf-8").read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def build_and_run():
    p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0 and ("LNK1236" in p.stdout or "LNK1000" in p.stdout):
        subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                           cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines()[-6:]:
            print("    " + line)
        return "build-failed"
    r = subprocess.run([os.path.join(ROOT, EXE)], cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
    fails = [l for l in r.stdout.splitlines() if "FAIL" in l]
    tail = [l for l in r.stdout.splitlines() if "checks," in l]
    print("  rc=%d  %s" % (r.returncode, tail[-1] if tail else "(crashed -- no summary)"))
    for l in fails[:8]:
        print("    " + l.strip())
    if r.returncode == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("M1 Slerp does not clamp t", F_MATH,
     "        t = Math::Clamp(t, 0.f, 1.f);\n"
     "\n"
     "        const float magnitude_a = a.length();",
     "        (void)t;\n"
     "\n"
     "        const float magnitude_a = a.length();"),

    ("M2 Slerp keeps the start magnitude", F_MATH,
     "        return direction * (magnitude_a + t * (magnitude_b - magnitude_a));",
     "        return direction * magnitude_a;"),

    ("M3 Slerp drops the zero-magnitude guard (NaN)", F_MATH,
     "        if (magnitude_a < 1e-5f || magnitude_b < 1e-5f)",
     "        if (false)"),

    ("M4 RotateTowards ignores the step budget", F_MATH,
     "        if (angle > max_radians_delta && angle > 1e-6f)",
     "        if (angle > 1e6f)"),

    ("M5 RotateTowards reads its budget as degrees", F_MATH,
     "                Quaternion::SLerp(max_radians_delta / angle, Quaternion::IDENTITY, rotation, true);",
     "                Quaternion::SLerp(max_radians_delta * Math_PI / 180.f / angle, Quaternion::IDENTITY, rotation, true);"),

    ("M6 RotateTowards snaps the magnitude too", F_MATH,
     "        return direction * MoveTowardsScalar(magnitude_current, magnitude_target, max_magnitude_delta);",
     "        return direction * magnitude_target;"),

    ("M7 ProjectOnPlane is just Project", F_MATH,
     "        return v - Project(v, plane_normal);",
     "        return Project(v, plane_normal);"),

    ("M8 Project goes back to the Ogre plane semantics", F_MATH,
     "        const float squared_length = on_normal.squaredLength();\n"
     "        if (squared_length < 1e-12f)\n"
     "        {\n"
     "            return Vector3::ZERO;\n"
     "        }\n"
     "        return on_normal * (v.dotProduct(on_normal) / squared_length);",
     "        return v.project(on_normal);"),

    ("M9 SignedAngle drops the sign", F_MATH,
     "        return unsigned_angle * sign;",
     "        return unsigned_angle;"),

    ("M10 Quaternion Normalize drops the zero guard (NaN)", F_MATH,
     "        if (squared < 1e-12f)\n"
     "        {\n"
     "            return Quaternion::IDENTITY;\n"
     "        }",
     "        if (false)\n"
     "        {\n"
     "            return Quaternion::IDENTITY;\n"
     "        }"),

    ("M11 Quaternion RotateTowards reads its budget as radians", F_MATH,
     "        return Slerp(from, to, step / angle);",
     "        return Slerp(from, to, (step / angle) * Math_PI / 180.f);"),

    ("M12 Quaternion Dot drops a term", F_MATH,
     "        return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;",
     "        return a.w * b.w + a.x * b.x + a.y * b.y;"),

    ("M13 Slerp is silently not exported", F_MATH,
     "    ZFUNCTION()\n    static Vector3 Slerp(",
     "    static Vector3 Slerp("),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".vqbak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
        print()
    print("---- summary ----")
    for n, r in results:
        print("%-58s %s" % (n, r))


if __name__ == "__main__":
    main()
