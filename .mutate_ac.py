"""Mutation harness for `AnimationCurve` / `Keyframe` / `WrapMode` (the AC / ACW / W slice).

Why this slice needs its own harness at all:

* The headline traps are all "wrong but plausible numbers", and each one is
  invisible under a naive fixture:
  - Dropping the `* dt` that turns a slope into a Hermite tangent (ACM04) only
    shows up on a segment whose dt != 1. Uniformly sampled fixtures -- which is
    what a curve test naturally looks like -- give the same answer either way.
  - `SmoothTangents` weighting by the same segment instead of the opposite one
    (ACM06/ACM07) needs UNEQUAL time spacing to separate.
  - `ClampForever` using the wrong tangent (ACM10) needs a curve whose first and
    last slopes differ; on a symmetric curve the swapped version is identical.
  - `Loop` vs `PingPong` (ACM11/ACM12) needs an ASYMMETRIC curve: on 0 -> 3 -> 0
    (which is what I first wrote) both give the same number at every time.
* `Linear` is the one place where a wrong tangent is NOT caught: setting both
  tangents to the slope also produces a straight line (b + c collapses to u).
  So there is deliberately no mutant for "Linear's endpoint tangents are wrong";
  the mutant that IS caught is the slope's divisor (ACM01).
* Publishing is entirely invisible to the C++ arithmetic tests: `WrapMode` only
  exists to scripts as a ZENUM (ACW4), and the two `declare interface` lines are
  the only reason `AnimationCurve` type-checks in a user project (ACW9).

Known coverage gap (deliberately not written as a mutant):

* `AddKey` returning a curve is value semantics; a script that forgets to keep
  the result silently gets an empty curve. That is a USAGE error, not an
  implementation one, so no mutant can represent it -- it is documented in
  ScriptAnimationCurve.h instead.

Run:  py .mutate_ac.py            (all)
      py .mutate_ac.py ACM04      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

CURVE_H = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\AnimationCurve.h")
FACADE_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Scripting\ScriptAnimationCurve\ScriptAnimationCurve.cpp")
FACADE_H = os.path.join(
    ROOT, r"engine\Source\Runtime\Scripting\ScriptAnimationCurve\ScriptAnimationCurve.h")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
PARITY_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Test\UnityParitySmokeTest.cpp")

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
# LNK1236（md5 两份相同照样报）。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    olds = old if isinstance(old, list) else [old]
    news = new if isinstance(new, list) else [new]
    for o, n in zip(olds, news):
        if o not in text:
            raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, o))
        text = text.replace(o, n, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


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
        raise SystemExit("END OF FUNC NOT FOUND: %r (marker %r)" % (func_sig, marker))
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
        for line in (p.stdout or "").splitlines()[-3:]:
            print("    " + line)
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


# --- 锚点 -------------------------------------------------------------------
HERMITE_END = "\n    float EvaluateOutside("
SMOOTH_END = "\n    // ---- 求值"
EVAL_END = "\nprivate:"
FACADE_LINEAR_END = "\n    ::AnimationCurve AnimationCurve::EaseInOut"

MUTATIONS = [
    # ---- 工厂 ---------------------------------------------------------------
    # 斜率没除时长 ⇒ 切线大了 10 倍。端点仍然经过给定值，只有中间能看出来。
    ("ACM01 Linear's slope forgets to divide by the time span", FACADE_CPP,
     "::AnimationCurve AnimationCurve::Linear",
     "        const float slope = (value_end - value_start) / (time_end - time_start);",
     "        const float slope = (value_end - value_start);",
     FACADE_LINEAR_END),

    # EaseInOut 的切线填成斜率 ⇒ 退化成直线，不再是 smoothstep。
    ("ACM02 EaseInOut uses the slope as its tangents (it stops easing)", FACADE_CPP,
     "::AnimationCurve AnimationCurve::EaseInOut",
     "        curve.AddKey(::Keyframe(time_start, value_start, 0.f, 0.f));\n"
     "        curve.AddKey(::Keyframe(time_end, value_end, 0.f, 0.f));",
     "        const float slope = (value_end - value_start) / (time_end - time_start);\n"
     "        curve.AddKey(::Keyframe(time_start, value_start, slope, slope));\n"
     "        curve.AddKey(::Keyframe(time_end, value_end, slope, slope));"),

    # Constant 的第二帧值写错：末点不再是那个恒定值。
    ("ACM03 Constant's second key loses the value", FACADE_CPP,
     "::AnimationCurve AnimationCurve::Constant",
     "        curve.AddKey(::Keyframe(time_end, value, 0.f, 0.f));",
     "        curve.AddKey(::Keyframe(time_end, 0.f, 0.f, 0.f));"),

    # ---- ⚠️ 头条：Hermite 里那个 `* dt` --------------------------------------
    # 只在这一段 dt != 1 的曲线上分得开（AC8 用的是 dt = 2）。
    ("ACM04 the tangent is not scaled by dt before entering the Hermite basis",
     CURVE_H, "static float Hermite",
     "        const float m0 = k1.outTangent * dt;\n"
     "        const float m1 = k2.inTangent * dt;",
     "        const float m0 = k1.outTangent;\n"
     "        const float m1 = k2.inTangent;",
     HERMITE_END),

    # ---- SmoothTangents -----------------------------------------------------
    # 权重取**自己**那一段而不是对面那一段。时间间距相等时两者相同。
    ("ACM05 SmoothTangents weights each slope by its OWN segment", CURVE_H,
     "void SmoothTangents",
     "        const float tangent = (s1 * dt2 + s2 * dt1) / (dt1 + dt2);",
     "        const float tangent = (s1 * dt1 + s2 * dt2) / (dt1 + dt2);",
     SMOOTH_END),

    # 退化成算术平均。
    ("ACM06 SmoothTangents degrades to the arithmetic mean", CURVE_H,
     "void SmoothTangents",
     "        const float tangent = (s1 * dt2 + s2 * dt1) / (dt1 + dt2);",
     "        const float tangent = (s1 + s2) * 0.5f;",
     SMOOTH_END),

    # ---- 编辑 ---------------------------------------------------------------
    # 不排序：Evaluate 依赖有序，乱序得到"能跑但值不对"。
    ("ACM07 AddKey appends instead of inserting in time order", CURVE_H,
     "void AddKey(const Keyframe& key)",
     "        keys.insert(LowerBound(key.time), key);",
     "        keys.push_back(key);"),

    # 不去重：同一时刻两帧 ⇒ 那一段 dt 为 0。
    ("ACM08 AddKey stops replacing a key at an existing time", CURVE_H,
     "void AddKey(const Keyframe& key)",
     "        for (Keyframe& existing : keys)\n"
     "        {\n"
     "            if (existing.time == key.time)\n"
     "            {\n"
     "                existing = key;\n"
     "                return;\n"
     "            }\n"
     "        }\n", ""),

    ("ACM09 MoveKey does not re-sort", CURVE_H, "void MoveKey",
     "        keys[static_cast<std::size_t>(index)] = key;\n        SortKeys();",
     "        keys[static_cast<std::size_t>(index)] = key;"),

    # ---- 环绕模式 -----------------------------------------------------------
    # ⚠️ 用**反**了那一条切线：对称曲线上分不出，AC26/AC27 用的是首末斜率不同
    # 的曲线。
    ("ACM10 ClampForever picks the wrong tangent at each end (in/out swapped)",
     CURVE_H, "float EvaluateOutside",
     "                const float tangent = before ? edge.outTangent : edge.inTangent;",
     "                const float tangent = before ? edge.inTangent : edge.outTangent;"),

    # Loop 用成 PingPong 的周期（2 * duration）。
    ("ACM11 Loop wraps on the PingPong period", CURVE_H, "float EvaluateOutside",
     "                const float period = (mode == WrapMode::Loop) ? duration : (2.f * duration);",
     "                const float period = 2.f * duration;"),

    # PingPong 用成 Loop 的周期 ⇒ 它退化成 Loop（不镜像）。
    ("ACM12 PingPong wraps on the Loop period (it stops mirroring)", CURVE_H,
     "float EvaluateOutside",
     "                const float period = (mode == WrapMode::Loop) ? duration : (2.f * duration);",
     "                const float period = duration;"),

    # 默认 wrap 从"保持端点值"变成"按切线外延"。
    ("ACM13 the default wrap mode becomes ClampForever", CURVE_H,
     "    WrapMode preWrapMode {WrapMode::Once};\n    WrapMode postWrapMode {WrapMode::Once};",
     "    WrapMode preWrapMode {WrapMode::ClampForever};\n"
     "    WrapMode postWrapMode {WrapMode::ClampForever};"),

    # ---- 退化输入的判据 -----------------------------------------------------
    # 不判 NaN：它会一路走到"没找到段"的兜底，而那和"真的没有段"分不开。
    ("ACM14 Evaluate stops guarding against a non-finite time", CURVE_H,
     "float Evaluate(float time) const",
     "        if (!std::isfinite(time))\n"
     "        {\n"
     "            return 0.f;\n"
     "        }\n", "", EVAL_END),

    # duration 取成末帧时间而不是跨度的差。
    ("ACM15 GetDuration returns the end time instead of the span", CURVE_H,
     "float GetDuration() const",
     "        return keys.back().time - keys.front().time;",
     "        return keys.back().time;"),

    # ---- 脚本侧的形状 -------------------------------------------------------
    # AddKey 不接返回值 ⇒ 曲线一直是空的。这里模拟"忘了加"。
    ("ACM16 the facade's AddKey does not add anything", FACADE_CPP,
     "::AnimationCurve AnimationCurve::AddKey(const ::AnimationCurve& curve, float time, float value)",
     "        out.AddKey(time, value);", ""),

    # SetKeys 不排序：脚本给的数组未必有序。
    ("ACM17 SetKeys keeps whatever order the script handed over", FACADE_CPP,
     "::AnimationCurve AnimationCurve::SetKeys",
     "        std::stable_sort(out.keys.begin(), out.keys.end(),\n"
     "                         [](const ::Keyframe& a, const ::Keyframe& b) { return a.time < b.time; });\n",
     ""),

    # ClearKeys 顺手把 wrap 模式也清了（Unity 不会）。
    ("ACM18 ClearKeys also resets the wrap modes", FACADE_CPP,
     "::AnimationCurve AnimationCurve::ClearKeys",
     "        ::AnimationCurve out = curve;\n        out.ClearKeys();\n        return out;",
     "        ::AnimationCurve out = curve;\n        out.ClearKeys();\n"
     "        out.preWrapMode = WrapMode::Default;\n"
     "        out.postWrapMode = WrapMode::Default;\n        return out;"),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ 这两个只有源码级门（ACW4 / ACW9）能看见：C++ 算术全部照常。
    ("ACM19 WrapMode is no longer published as a script enum", CURVE_H,
     "ZENUM()\nenum class WrapMode : int", "enum class WrapMode : int"),

    ("ACM20 the .d.ts declarations are gone", BINDING_CPP,
     '        "declare interface Keyframe {\\n"\n', ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_ac.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_ac.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".acbak")
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
                with io.open(os.path.join(ROOT, ".mutate_ac.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-64s %s\n" % (name, verdict))
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
