"""Mutation harness for the Mathf completion slice (AI/AJ checks)."""
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
    ("M1 Round breaks ties away from zero (std::round)", F_MATH,
     "    static float Round(float value) { return std::rint(value); }",
     "    static float Round(float value) { return std::round(value); }"),

    ("M2 Repeat is fmod (negatives stay negative)", F_MATH,
     "        return Clamp(t - std::floor(t / length) * length, 0.f, length);",
     "        return std::fmod(t, length);"),

    ("M3 DeltaAngle is a plain subtraction", F_MATH,
     "        float delta = Repeat(target - current, 360.f);\n"
     "        if (delta > 180.f)\n"
     "        {\n"
     "            delta -= 360.f;\n"
     "        }\n"
     "        return delta;",
     "        return target - current;"),

    ("M4 LerpAngle ignores the short arc", F_MATH,
     "        return a + delta * Clamp01(t);",
     "        return a + (b - a) * Clamp01(t);"),

    ("M5 InverseLerp drops the zero-length guard (NaN)", F_MATH,
     "        if (a == b)\n        {\n            return 0.f;\n        }\n"
     "        return Clamp01((value - a) / (b - a));",
     "        return Clamp01((value - a) / (b - a));"),

    ("M6 SmoothStep does not clamp t", F_MATH,
     "        t = Clamp01(t);\n        t = t * t * (3.f - 2.f * t);",
     "        t = t * t * (3.f - 2.f * t);"),

    ("M7 Log is base 10", F_MATH,
     "    static float Log(float value) { return std::log(value); }",
     "    static float Log(float value) { return std::log10(value); }"),

    ("M8 Atan2 takes (x, y)", F_MATH,
     "    static float Atan2(float y, float x) { return std::atan2(y, x); }",
     "    static float Atan2(float y, float x) { return std::atan2(x, y); }"),

    ("M9 IsPowerOfTwo accepts 0", F_MATH,
     "    static bool IsPowerOfTwo(int32_t value) { return (value > 0) && ((value & (value - 1)) == 0); }",
     "    static bool IsPowerOfTwo(int32_t value) { return ((value & (value - 1)) == 0); }"),

    ("M10 NextPowerOfTwo forgets the pre-decrement", F_MATH,
     "        value -= 1;\n        value |= value >> 16;",
     "        value |= value >> 16;"),

    ("M11 GammaToLinearSpace is a plain pow(x, 2.2)", F_MATH,
     "        if (value <= 0.04045f)\n"
     "        {\n"
     "            return value / 12.92f;\n"
     "        }\n"
     "        if (value < 1.f)\n"
     "        {\n"
     "            return std::pow((value + 0.055f) / 1.055f, 2.4f);\n"
     "        }\n"
     "        return std::pow(value, 2.2f);",
     "        return std::pow(value, 2.2f);"),

    ("M12 Approximately drops the absolute term", F_MATH,
     "        return Math::abs(b - a) < Max(1e-6f * Max(Math::abs(a), Math::abs(b)), Epsilon() * 8.f);",
     "        return Math::abs(b - a) < 1e-6f * Max(Math::abs(a), Math::abs(b));"),

    ("M13 SmoothStep is silently not exported", F_MATH,
     "    ZFUNCTION()\n    static float SmoothStep(",
     "    static float SmoothStep("),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".mfbak")
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
