"""Mutation harness for `LayerMask` + the `Vector3` static face + the `Mathf` tail.

Everything in this slice is a *naming* gap rather than a new algorithm: the layer
table has always lived in `TagLayerManager`, and `Slerp` / `RotateTowards` /
`ProjectOnPlane` / `SignedAngle` have always lived in `VectorMath`. What did not
exist was Unity's spelling for them. That shapes the mutants:

* Most of them are "the facade forwards to the wrong thing" (VXM01-VXM04). Each
  wrong target is a *plausible* function: `Lerp` for `Slerp`, `Project` for
  `ProjectOnPlane`, `Angle` for `SignedAngle`. All four still return a Vector3 or
  a float of the right magnitude; only the specific property in the test
  (magnitude 1 vs 0.7071, the z component, the sign) tells them apart.
* The LayerMask ones are about *failure direction*, which is the whole reason the
  facade exists at all: `NameToLayer` returning 0 instead of -1 (LMM01) turns a
  typo into "raycast hits Default", and `GetMask` returning -1 instead of
  skipping (LMM02) turns it into "raycast hits everything". Both look like
  physics being noisy, not like a misspelled layer name.
* LMM04 is the sign-bit trap: `1 << 31` is negative in a 32-bit signed int, so a
  facade that masks off the top bit silently loses the highest layer.

Known coverage gap (deliberately NOT written as a mutant):

* `Mover.ts` is not recompiled by this harness, so Z2-Z5 have no mutants of
  their own. Their numbers all come from C++ functions that do: `0|5|-1|UI|33|32`
  from LMM01/LMM02/LMM03, `1.0000|0.7071` from VXM01, `0.0000` from VXM03, and
  `90.000|-90.000` from VXM04.

Run:  py .mutate_lm.py            (all)
      py .mutate_lm.py LMM01      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

MASK_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptLayerMask\ScriptLayerMask.h")
MASK_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptLayerMask\ScriptLayerMask.cpp")
VEC_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.h")
VEC_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.cpp")
MATH_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptMath\ScriptMath.h")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n    }\n"

# ⚠️ 一次只编一个 target：同时给 `--target A B` 会让 MSBuild 并行跑，某个 target
# 的 post-build 往 bin/Debug/ZRuntime.lib 拷贝时正好撞上另一个的链接 —— 间歇性
# LNK1236。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")

# ⚠️ `ScriptLayerMask.cpp` 这几个函数**用默认结束标记就够**：函数体里那些 `if` /
# `for` 的右花括号是 8 空格缩进的，只有函数自己的收尾是 4 空格。给显式标记反而会
# 把要改的那行切到区域**外面**（标记从那行的 `\n` 起算，区域就到上一行为止）——
# 第一轮就是这么报 "PATTERN NOT FOUND" 的。


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    """Replace only inside one function body (see .mutate_tc.py for why)."""
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else END_OF_FUNC
    j = text.find(marker, i)
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
        for target in TARGETS:
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
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # ---- ⚠️ 头条：失败方向 ---------------------------------------------------
    # 找不到层名时返回 0 而不是 -1。0 是 Default —— 最热闹的那一层 —— 于是"层名
    # 拼错"变成"射线打到 Default 上的东西"，而射线**确实有结果**，完全看不出来。
    ("LMM01 NameToLayer returns 0 (Default) instead of -1 for an unknown name",
     MASK_CPP, "int32_t LayerMask::NameToLayer(",
     "        return tag_layer->NameToLayer(name);",
     "        const int32_t layer = tag_layer->NameToLayer(name);\n"
     "        return (layer < 0) ? 0 : layer;"),

    # ⚠️ 互补的一半：未知层名让**整张掩码作废**（返回 -1 = 所有层）。ZEngine 选择
    # 跳过（少打一层，"这个物体不响应点击"），因为返回 -1 的表现是"射线打到所有
    # 东西"，离"层名拼错了"远得多。LM9 钉住这个选择。
    ("LMM02 GetMask returns -1 (every layer) when a name is unknown, instead of skipping it",
     MASK_CPP, "int32_t LayerMask::GetMask(",
     "        return static_cast<int32_t>(tag_layer->GetMask(names));",
     "        for (const eastl::string& n : names)\n        {\n"
     "            if (tag_layer->NameToLayer(n) < 0) { return -1; }\n        }\n"
     "        return static_cast<int32_t>(tag_layer->GetMask(names));"),

    # 掩码恒为 0：射线什么都打不到。
    ("LMM03 GetMask always returns 0", MASK_CPP, "int32_t LayerMask::GetMask(",
     "        return static_cast<int32_t>(tag_layer->GetMask(names));",
     "        return 0;"),

    # ⚠️ 第 31 位：`1 << 31` 在 32 位有符号整数里是负数。把符号位抹掉的实现，
    # 在 0..30 层上完全正确，只在最高那一层上静默失效。
    ("LMM04 MaskContainsLayer drops the sign bit (layer 31 disappears)",
     MASK_CPP, "bool LayerMask::MaskContainsLayer(",
     "        return TagLayerManager::MaskContainsLayer(static_cast<uint32_t>(mask), layer);",
     "        return TagLayerManager::MaskContainsLayer(\n"
     "            static_cast<uint32_t>(mask) & 0x7FFFFFFFu, layer);"),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ 只有源码门 LMW2 看得见：少一个 ZFUNCTION 不影响 C++，但用户工程里那个
    # 方法根本不存在 —— 而"方法不存在"在运行时是静默无事发生。
    ("LMM05 a LayerMask ZFUNCTION is dropped (C++ keeps working, the script name is gone)",
     MASK_H,
     "        ZFUNCTION()\n        static bool MaskContainsLayer(int32_t mask, int32_t layer);",
     "        static bool MaskContainsLayer(int32_t mask, int32_t layer);"),

    # ---- Vector3 静态面：门面转发到了"看起来合理"的另一个函数 ----------------
    # ⚠️ `Lerp` 是 `Slerp` 最像的错目标：方向都在那条 45° 射线上，只有**长度**
    # 分得开（1.0000 vs 0.7071）。VX0 就是那个判据。
    ("VXM01 Vector3.Slerp forwards to Lerp instead of Slerp (the magnitude collapses)",
     VEC_CPP, "::Vector3 Vector3::Slerp(",
     "        return VectorMath::Slerp(a, b, t);",
     "        return VectorMath::Lerp(a, b, t);"),

    # 两个 delta 换位置：角度预算变成了长度预算 —— 表现为"转得特别慢"。
    ("VXM02 Vector3.RotateTowards swaps its two budget arguments",
     VEC_CPP, "::Vector3 Vector3::RotateTowards(",
     "        return VectorMath::RotateTowards(current, target, max_radians_delta, "
     "max_magnitude_delta);",
     "        return VectorMath::RotateTowards(current, target, max_magnitude_delta, "
     "max_radians_delta);"),

    # 投影到**法线上**而不是平面上：结果仍然是个像样的向量。
    ("VXM03 Vector3.ProjectOnPlane forwards to Project",
     VEC_CPP, "::Vector3 Vector3::ProjectOnPlane(",
     "        return VectorMath::ProjectOnPlane(v, plane_normal);",
     "        return VectorMath::Project(v, plane_normal);"),

    # ⚠️ 少了符号的 `Angle` 是最阴的一种：0..180 的正数，绝对值与正确答案**相同**。
    # 只有"换顺序必须换符号"那条（VX11）看得见。
    ("VXM04 Vector3.SignedAngle forwards to Angle (it loses the sign)",
     VEC_CPP, "float Vector3::SignedAngle(",
     "        return VectorMath::SignedAngle(from, to, axis);",
     "        return VectorMath::Angle(from, to);"),

    # ---- 发布面 ---------------------------------------------------------------
    ("VXM05 the Vector3.SignedAngle ZFUNCTION is dropped (C++ keeps working, the script name is gone)",
     VEC_H,
     "        ZFUNCTION()\n        static float SignedAngle(const ::Vector3& from, "
     "const ::Vector3& to, const ::Vector3& axis);",
     "        static float SignedAngle(const ::Vector3& from, const ::Vector3& to, "
     "const ::Vector3& axis);"),

    # ---- Mathf.PerlinNoise1D --------------------------------------------------
    # 拿 2D 噪声的 y=0 切片来顶替：整数点上**也**是 0.5（fade(0) = 0），所以
    # "整数点是 0.5"这条判据抓不住它 —— 只有黄金数（MF1 / MF2）能。
    ("MFM01 PerlinNoise1D is a slice of the 2D noise instead of real 1D noise",
     MATH_H, "static float PerlinNoise1D(float x)",
     "        const float n0 = static_cast<float>(hash(x0)) * fx;",
     "        return PerlinNoise(x, 0.f);\n"
     "        const float n0 = static_cast<float>(hash(x0)) * fx;"),

    # fade 从五次退化成线性：仍然连续、仍然确定性，只是不那么"圆"。
    ("MFM02 PerlinNoise1D fades linearly instead of with the quintic curve",
     MATH_H, "static float PerlinNoise1D(float x)",
     "        auto fade = [](float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); };",
     "        auto fade = [](float t) { return t; };"),

    # 少了归一化：值域收窄一半，仍然在 [0,1] 内（所以"值域"那条判据抓不住）。
    ("MFM03 PerlinNoise1D forgets the 2x normalisation",
     MATH_H, "static float PerlinNoise1D(float x)",
     "        return 0.5f + 0.5f * Clamp(value * 2.f, -1.f, 1.f);",
     "        return 0.5f + 0.5f * Clamp(value, -1.f, 1.f);"),

    # 哈希恒定：噪声变成一条周期 1 的固定曲线，仍然连续、仍然确定性。
    ("MFM04 PerlinNoise1D's hash always picks the same gradient",
     MATH_H, "static float PerlinNoise1D(float x)",
     "            return ((h ^ (h >> 16)) & 1u) ? 1 : -1;",
     "            return 1;"),
]

LOCK = os.path.join(BAK, "zengine_mutate_lm.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_lm.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            end_marker = None
            if len(entry) == 6:
                name, path, func_sig, old, new, end_marker = entry
            elif len(entry) == 5:
                name, path, func_sig, old, new = entry
            else:
                name, path, old, new = entry
                func_sig = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            # 备份**工作副本**（不是 HEAD）：这次的改动还没提交。
            bak = os.path.join(BAK, os.path.basename(path) + ".lmbak")
            shutil.copy2(path, bak)
            try:
                try:
                    if func_sig is not None:
                        mutate_in(path, func_sig, old, new, end_marker)
                    else:
                        mutate(path, old, new)
                except SystemExit as e:
                    print("  HARNESS ERROR: %s" % e)
                    results.append((name, "harness error (anchor not found)"))
                    continue
                verdict = "BUILD FAILED"
                for _ in range(3):
                    verdict = build_and_run()
                    if verdict != "BUILD FAILED":
                        break
                    print("  (retrying -- flaky MSBuild)")
                if verdict == "BUILD FAILED":
                    verdict = "invalid (does not build)"
                results.append((name, verdict))
                with io.open(os.path.join(ROOT, ".mutate_lm.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
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
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
