"""Mutation harness for `SmoothDamp` (the SD / SDW / Z slice).

Why this slice needs its own harness:

SmoothDamp is the worst case for "looks right, is wrong". Every plausible bug
produces a curve that is still smooth, still converges, and still never
overshoots visibly:

* `std::exp(-x)` instead of Unity's polynomial (SDM01). Same shape, 3% off.
  The only thing that sees it is the 1e-6 golden number.
* `maxSpeed * deltaTime` instead of `maxSpeed * smoothTime` (SDM02). Still
  "limits the speed", but the limit now tracks the frame rate -- so a 30Hz
  machine damps differently from a 144Hz one. This is the mutation I most
  expect someone to write by accident, because `maxSpeed * dt` is what
  "speed limit" colloquially means.
* Dropping the overshoot guard (SDM03): the value oscillates around the target
  in a way that only shows up with a large velocity and a large dt.
* `SmoothDampAngle` skipping `DeltaAngle` (SDM04): 350 -> 10 still converges to
  10, it just goes the long way round through 180.
* A whole-vector speed limit instead of per-component (SDM05): still smooth,
  still converges, and for axis-aligned motion it is *identical*. Only the
  diagonal fixture (SD18) tells them apart.
* Not carrying the velocity (SDM06): the caller almost always does
  `pos = r.value` and forgets `vel = r.velocity`. That is not a bug in the
  engine, so there is no mutant for it -- but it is the reason the result is a
  struct rather than a plain float, which SDM07/SDM08 guard.

Known coverage gap (deliberately NOT written as a mutant):

* `Mover.ts` is not recompiled by this harness, so Z1 has no mutant of its own.
  Its numbers come from C++ functions that do: `0.002061` from SDM01's sibling,
  `0.001031` and `2.061` from SDM02's sibling, `350.041` from SDM04, and the
  trailing `0` from SDM09.

Round 1 was 17/19 -- two survivors, both of them real test gaps:

* SDM06 (the dt<=0 early-out in `Step`) survived because with dt == 0 the
  formula *accidentally* computes "hold": x == 0 so exp == 1, temp == 0, and
  output == target + change == current. The only corner where the guard fires
  at dt == 0 is `current == target`, and there `out_velocity = 0/0` becomes
  NaN -- while `value` is still correct. Fixed by SD10b.
* SDM10 (the dt<=0 sentinel in `Mathf::SmoothDamp`) survived because the only
  dt<=0 test ran while the engine delta was *also* 0, so "fall back to
  Time.deltaTime" and "do nothing" both give "hold". Fixed by SD10d/SD10e,
  which set a real engine delta first.

Run:  py .mutate_sd.py            (all)
      py .mutate_sd.py SDM01      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

DAMP_H = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\SmoothDamp.h")
MATH_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptMath\ScriptMath.h")
VEC_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")

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


# `SmoothDampDetail::Step` 的结束标记：默认 `"\n    }\n"` 会在第一个 4 空格缩进的
# 右花括号处停下（这里是 `ClampSmoothTime`，在 Step **之前**），所以必须显式给。
STEP_END = "\n        return output;\n    }\n"
# `Mathf::SmoothDamp` 的结束标记：它的函数体里有一个 4 空格缩进的 `if` 右花括号，
# 默认的 `END_OF_FUNC` 会停在那儿（在要改的那行**之前**）。
MATH_END = "\n        return out;\n    }\n"
# `Vector3::SmoothDamp` 同理（函数体开头就是 `if (!(delta_time > 0.f)) { ... }`）。
VEC_END = "\n        return out;\n    }\n"
ANGLE_END = "\n    }\n"

MUTATIONS = [
    # ---- ⚠️ 头条：多项式 vs std::exp ----------------------------------------
    # Unity 的 `exp` 是 `1/(1 + x + 0.48x² + 0.235x³)`。换成 `std::exp(-x)` 之后
    # 曲线仍然平滑、仍然收敛、仍然不过冲 —— 只有 SD0 / SD1 / SD3 的 1e-6 黄金数
    # 看得见（0.002061 → 0.002126，0.249485 → 0.249469）。
    ("SDM01 the damping term becomes std::exp(-x) instead of Unity's polynomial",
     DAMP_H, "inline float Step(",
     "        const float exp = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);",
     "        const float exp = std::exp(-x);", STEP_END),

    # 掉一个系数：0.235 → 0.2350 看起来一样，其实是"少了一项"。
    ("SDM02b the cubic coefficient is dropped (0.235 -> 0)",
     DAMP_H, "inline float Step(",
     "0.48f * x * x + 0.235f * x * x * x", "0.48f * x * x", STEP_END),

    # ---- ⚠️ maxSpeed 的上限 --------------------------------------------------
    # 正确是 `maxSpeed * smoothTime`（0.5）；写成 `maxSpeed * deltaTime`（1/60）
    # 之后限速仍然存在，但**随帧率变** —— 30Hz 与 60Hz 的阻尼不一样，而任何单帧
    # 看都是"确实在限速"。SD5（黄金数）+ SD6b（跨帧率比对）一起抓。
    ("SDM02 the speed limit is maxSpeed * deltaTime (it tracks the frame rate)",
     DAMP_H, "inline float Step(",
     "            const float max_change = max_speed * smooth_time;",
     "            const float max_change = max_speed * delta_time;", STEP_END),

    # 完全不限速：SD5 的黄金数变三个数量级。
    ("SDM02c maxSpeed is ignored entirely",
     DAMP_H, "inline float Step(",
     "            const float max_change = max_speed * smooth_time;",
     "            const float max_change = max_speed * smooth_time * 1e30f;", STEP_END),

    # ---- 过冲守卫 ------------------------------------------------------------
    # 少了它，大初速 + 大 dt 会绕着目标来回抖；单看一帧仍然"在靠近"。
    ("SDM03 the overshoot guard is removed",
     DAMP_H, "inline float Step(",
     "        if ((original_to - current > 0.f) == (output > original_to))",
     "        if (false)", STEP_END),

    # 守卫的方向反了：正常靠近的每一帧都被钉住 ⇒ 一步到位（不是平滑）。
    ("SDM03b the overshoot guard fires on the wrong side",
     DAMP_H, "inline float Step(",
     "(original_to - current > 0.f) == (output > original_to)",
     "(original_to - current > 0.f) != (output > original_to)", STEP_END),

    # 钉住时速度清零写成"保留原速"：下一帧继续冲。
    ("SDM03c pinning keeps the old velocity instead of zeroing it",
     DAMP_H, "inline float Step(",
     "            out_velocity = (output - original_to) / delta_time;",
     "            out_velocity = current_velocity;", STEP_END),

    # ---- smoothTime 的夹取 ---------------------------------------------------
    # smoothTime = 0 直接当除数 ⇒ inf / NaN，而 NaN 一旦进位置就再也洗不掉。
    ("SDM04 ClampSmoothTime no longer clamps (divide by zero)",
     DAMP_H, "inline float ClampSmoothTime",
     "        return (smooth_time > 0.0001f) ? smooth_time : 0.0001f;",
     "        return smooth_time;"),

    # 夹到 1 而不是 0.0001：smoothTime = 0.5 的调用全变成 smoothTime = 1。
    ("SDM04b ClampSmoothTime clamps to 1 instead of 0.0001",
     DAMP_H, "inline float ClampSmoothTime",
     "        return (smooth_time > 0.0001f) ? smooth_time : 0.0001f;",
     "        return (smooth_time > 1.f) ? smooth_time : 1.f;"),

    # omega 的定义：2/smoothTime 写成 1/smoothTime ⇒ 慢一倍，仍然收敛。
    ("SDM05 omega is 1/smoothTime instead of 2/smoothTime",
     DAMP_H, "inline float Step(",
     "        const float omega = 2.f / smooth_time;",
     "        const float omega = 1.f / smooth_time;", STEP_END),

    # ---- dt <= 0 -------------------------------------------------------------
    # 暂停帧上按公式算 ⇒ 除以 0 ⇒ NaN。Unity 也是 NaN，但"不动"是可恢复的。
    ("SDM06 deltaTime <= 0 runs the formula anyway (NaN)",
     DAMP_H, "inline float Step(",
     "        if (!(delta_time > 0.f))\n        {\n"
     "            out_velocity = current_velocity;\n            return current;\n        }\n",
     "", STEP_END),

    # ---- SmoothDampAngle ------------------------------------------------------
    # 少了 `DeltaAngle`：350 → 10 走 -340 的长弧。仍然平滑、仍然收敛到 10°。
    ("SDM07 SmoothDampAngle skips DeltaAngle (it takes the long way round)",
     MATH_H, "static SmoothDampResult SmoothDampAngle(",
     "        return SmoothDamp(current, current + DeltaAngle(current, target), "
     "current_velocity,\n                          smooth_time, max_speed, delta_time);",
     "        return SmoothDamp(current, target, current_velocity,\n"
     "                          smooth_time, max_speed, delta_time);", ANGLE_END),

    # ---- Vector3：分量式 vs 整向量 ---------------------------------------------
    # ⚠️ 用"整向量限速"（先夹长度再算）的实现：轴向运动时与 Unity **完全一致**，
    # 只有 SD18 的对角夹具（合成速度是 √2 倍）分得开。
    ("SDM08 Vector3.SmoothDamp limits the whole vector instead of each component",
     VEC_CPP, "Vector3SmoothDampResult Vector3::SmoothDamp(",
     "        out.value.x = SmoothDampDetail::Step(current.x, target.x, current_velocity.x, "
     "smooth_time,\n                                             max_speed, delta_time, "
     "out.velocity.x);",
     "        const float scale = 1.f / std::sqrt(3.f);\n"
     "        out.value.x = SmoothDampDetail::Step(current.x, target.x, current_velocity.x, "
     "smooth_time,\n                                             max_speed * scale, "
     "delta_time, out.velocity.x);", VEC_END),

    # z 分量抄成了 x 的目标：已经在目标上的分量开始动（SD17）。
    ("SDM09 Vector3.SmoothDamp reads target.x for the z component",
     VEC_CPP, "Vector3SmoothDampResult Vector3::SmoothDamp(",
     "out.value.z = SmoothDampDetail::Step(current.z, target.z, current_velocity.z, "
     "smooth_time,\n                                             max_speed, delta_time, "
     "out.velocity.z);",
     "out.value.z = SmoothDampDetail::Step(current.z, target.x, current_velocity.z, "
     "smooth_time,\n                                             max_speed, delta_time, "
     "out.velocity.z);", VEC_END),

    # z 的速度写回了 x 的位置：SD16 / SD17 一起红。
    ("SDM09b Vector3.SmoothDamp writes z's velocity into x's",
     VEC_CPP, "Vector3SmoothDampResult Vector3::SmoothDamp(",
     "max_speed, delta_time, out.velocity.x);",
     "max_speed, delta_time, out.velocity.z);", VEC_END),

    # ---- 哨兵：deltaTime <= 0 取引擎当前帧 -------------------------------------
    # 少了它，TS 里传 0（"我没传这个参数"）会直接被当成一帧 0 秒 ⇒ 永远不动。
    ("SDM10 the deltaTime <= 0 sentinel no longer falls back to Time.deltaTime",
     MATH_H, "static SmoothDampResult SmoothDamp(",
     "        if (!(delta_time > 0.f))\n        {\n"
     "            delta_time = Time::GetDeltaTime();\n        }\n",
     "", MATH_END),

    # ⚠️ Vector3 版有自己的一份哨兵：Mathf 那份救不了它（两个 ZFUNCTION 是两个
    # 函数体）。SD10e 抓。
    ("SDM10b Vector3.SmoothDamp's deltaTime sentinel no longer falls back to Time.deltaTime",
     VEC_CPP, "Vector3SmoothDampResult Vector3::SmoothDamp(",
     "        if (!(delta_time > 0.f))\n        {\n"
     "            delta_time = Time::GetDeltaTime();\n        }\n",
     "", VEC_END),

    # ---- 发布面 ----------------------------------------------------------------
    # ⚠️ 只有源码门 SDW9 / 端到端 Z1 看得见：少了 `declare interface`，用户工程里
    # `SmoothDampResult` 这个名字根本不存在；少了 Converter，TS 拿到 undefined
    # 而 `String(undefined)` 照样能拼进探针字符串。
    ("SDM11 the SmoothDampResult .d.ts declaration is gone", BINDING_CPP,
     '        "declare interface SmoothDampResult {\\n"\n', ""),

    ("SDM12 the Vector3SmoothDampResult .d.ts declaration is gone", BINDING_CPP,
     '        "declare interface Vector3SmoothDampResult {\\n"\n', ""),

    # ⚠️ Mathf 与 Vector3 各写一份实现：两份迟早漂移，而漂移后的两条曲线都平滑。
    # 只有源码门 SDW1 / SDW3 看得见它们是不是同一个函数。
    ("SDM13 Mathf.SmoothDamp stops sharing the implementation with Vector3",
     MATH_H, "static SmoothDampResult SmoothDamp(",
     "        out.value = SmoothDampDetail::Step(current, target, current_velocity, "
     "smooth_time,\n                                           max_speed, delta_time, "
     "out.velocity);",
     "        const float omega = 2.f / (smooth_time > 0.0001f ? smooth_time : 0.0001f);\n"
     "        const float x = omega * delta_time;\n"
     "        const float e = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);\n"
     "        const float change = current - target;\n"
     "        const float temp = (current_velocity + omega * change) * delta_time;\n"
     "        out.velocity = (current_velocity - omega * temp) * e;\n"
     "        out.value = target + (change + temp) * e;", MATH_END),
]

LOCK = os.path.join(BAK, "zengine_mutate_sd.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_sd.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".sdbak")
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
                with io.open(os.path.join(ROOT, ".mutate_sd.log"), "a", encoding="utf-8") as lg:
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
