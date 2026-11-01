"""Mutation harness for the `Quaternion.Euler` / `q.eulerAngles` slice (QT / QW / U).

为什么这一片必须有自己的变异门禁：

* 欧拉角是**约定**不是算法。`FromEulerAngles` 里 `qz * qy * qx` 换成 `qx * qy * qz`
  照样编译、照样返回一个合法的旋转，物体照转 —— 只是转的不是你想要的那个方向。
  数值上唯一能看见它的只有 QT4/QT5/QT7 那几条**不对称**夹具。
* `Transform.eulerAngles` 与 `Quaternion.Euler` 一旦各写一份，两套约定就开始漂移。
  漂移本身不产生任何错误，所以 QW3/QW4/QW8/QW12 是**源码级**门，不是行为门 ——
  它们拦的是"实现正确但换了 owner"这种人类评审一定会放过的改动。
* QW13 是这一片新加的：反方向 `EulerFromQuaternion` 回退到 Ogre 遗产的
  `GetYaw/GetPitch/GetRoll`（Y-up 定义）时，**没有任何一条 QT 会红** ——
  读出来的还是"某个欧拉角"，只是不是你设进去的那个。QE17 证明这条门真的会响。
* QT24 / QT25 / QT26 / QT27 是补出来的：原来只有 QT22（Slerp 夹 t）。
  "Lerp 忘了夹"（QE06）和"Unclamped 传了 shortest_path=true"（QE07/QE08）
  都是完全静默的 —— 取反是同一个旋转，看朝向分不出来，只能看 dot 的符号。
* QE12/QE13/QE14 是门本身的门：把扫描锚点写坏，让门"什么也没扫到"。
  少了这三条，"门从不响"和"代码从没错"是分不开的。

Run:  py .mutate_qe.py            (all)
      py .mutate_qe.py QE01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

QUAT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\Quaternion.cpp")
TRANSFORM_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Component\Transform\Transform.cpp")
SCRIPTVEC_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptVector\ScriptVector.cpp")
PARITY_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Function\Framework\Test\UnityParitySmokeTest.cpp")
MOVER_JS = os.path.join(
    ROOT, r"engine\Source\Runtime\Scripting\ScriptVM\Test\TscOutput\Mover.js")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker):
    """只在一个函数体内替换。

    ⚠️ 默认锚点 `"\\n    }\\n"` 在 `ToEulerAngles` 里会先撞上 `if (std::fabs(...))`
    那个块的收尾大括号，所以这里一律显式传 `end_marker`。
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    j = text.find(end_marker, i)
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
                                "--config", "Debug", "--parallel", "1"],
                               cwd=os.path.join(ROOT, "build"),
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
    if (run(PARITY_EXE) | run(COMP_EXE)) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


# ---- 锚点 ------------------------------------------------------------------
FROM_EULER_SIG = "Quaternion Quaternion::FromEulerAngles(const Vector3& euler_degrees)"
FROM_EULER_END = "\n//-----------------------------------------------------------------------\n// R = Rz"
TO_EULER_SIG = "Vector3 Quaternion::ToEulerAngles(const Quaternion& rotation)"
TO_EULER_END = "\nQuaternion Quaternion::SLerp("

EULER_TO_QUAT_SIG = "    Quaternion EulerToQuaternion(const Vector3& euler_degrees)"
EULER_TO_QUAT_END = "\n    Vector3 EulerFromQuaternion"
EULER_FROM_QUAT_SIG = "    Vector3 EulerFromQuaternion(const Quaternion& rotation)"
EULER_FROM_QUAT_END = "\n    bool IsAnyNaN"

SCRIPT_EULER_SIG = "    ::Quaternion Quaternion::Euler(float x, float y, float z)"
SCRIPT_EULER_END = "\n    ::Quaternion Quaternion::EulerVector"

MUTATIONS = [
    # ---- ⚠️ 头条：合成顺序 --------------------------------------------------
    # `qx * qy * qz` 是一个完全合法的旋转，物体照转，只是转错方向。只有不对称
    # 夹具（20/30/40）+ 基向量落点（QT7/8/9）能看见。
    ("QE01 FromEulerAngles composes qx*qy*qz instead of qz*qy*qx",
     QUAT_CPP, FROM_EULER_SIG,
     "    Quaternion result = qz * qy * qx;",
     "    Quaternion result = qx * qy * qz;",
     FROM_EULER_END),

    # ---- 反向：x 与 z 对调 --------------------------------------------------
    # 对称夹具 (30,30,30) 下这条是绿的 —— 这正是 QT10 用两组不对称值的原因。
    ("QE03 ToEulerAngles swaps x and z", QUAT_CPP, TO_EULER_SIG,
     "    return Vector3(Math::RadiansToDegrees(Math::atan2(r[2][1], r[2][2]).valueRadians()),\n"
     "                   Math::RadiansToDegrees(Math::asin(sin_b).valueRadians()),\n"
     "                   Math::RadiansToDegrees(Math::atan2(r[1][0], r[0][0]).valueRadians()));",
     "    return Vector3(Math::RadiansToDegrees(Math::atan2(r[1][0], r[0][0]).valueRadians()),\n"
     "                   Math::RadiansToDegrees(Math::asin(sin_b).valueRadians()),\n"
     "                   Math::RadiansToDegrees(Math::atan2(r[2][1], r[2][2]).valueRadians()));",
     TO_EULER_END),

    # 万向锁：把 y 记成 0（不按 Unity 的"+/-90 全部记到 y"惯例）。
    ("QE04 the gimbal-lock branch loses the +/-90 on y", QUAT_CPP, TO_EULER_SIG,
     "        return Vector3(0.0f, Math::RadiansToDegrees(b),",
     "        return Vector3(0.0f, 0.0f,",
     TO_EULER_END),

    # ---- ⚠️ 第二套约定：Transform 就地展开 -----------------------------------
    # 行为完全正确（同样的 qz*qy*qx），所以没有任何一条 QT 会红 —— 只有 QW3/QW4
    # 这两条源码门能看见。这就是"实现对了但换了 owner"的形状。
    ("QE05 Transform inlines its own axis composition again (behaviour identical)",
     TRANSFORM_CPP, EULER_TO_QUAT_SIG,
     "        return Quaternion::FromEulerAngles(euler_degrees);",
     "        Quaternion qx; qx.FromAngleAxis(Radian(Rad(euler_degrees.x)), Vector3::UNIT_X);\n"
     "        Quaternion qy; qy.FromAngleAxis(Radian(Rad(euler_degrees.y)), Vector3::UNIT_Y);\n"
     "        Quaternion qz; qz.FromAngleAxis(Radian(Rad(euler_degrees.z)), Vector3::UNIT_Z);\n"
     "        return qz * qy * qx;",
     EULER_TO_QUAT_END),

    # ---- ⚠️ 同一个：脚本面各写一份 ------------------------------------------
    # 顺序也写对，所以 QT13 照样绿。只有 QW8 会响。
    ("QE06 the script face Euler builds its own copy (behaviour identical)",
     SCRIPTVEC_CPP, SCRIPT_EULER_SIG,
     "        return ::Quaternion::FromEulerAngles(::Vector3(x, y, z));",
     "        return QuaternionMath::AngleAxis(z, ::Vector3::UNIT_Z) *\n"
     "               QuaternionMath::AngleAxis(y, ::Vector3::UNIT_Y) *\n"
     "               QuaternionMath::AngleAxis(x, ::Vector3::UNIT_X);",
     SCRIPT_EULER_END),

    # ---- ⚠️ 夹不夹 t：这是真 Unity 差异 -------------------------------------
    ("QE07 Quaternion.Slerp stops clamping t", SCRIPTVEC_CPP,
     "        return QuaternionMath::Slerp(a, b, ClampT(t));",
     "        return QuaternionMath::Slerp(a, b, t);"),

    # ⚠️ 没有 QT24 的话这条是 NOT CAUGHT：ClampT 加在 Slerp 上、Lerp 抄漏了，
    # 是最可能发生的一种。
    ("QE08 Quaternion.Lerp stops clamping t", SCRIPTVEC_CPP,
     "        return QuaternionMath::Nlerp(a, b, ClampT(t));\n",
     "        return QuaternionMath::Nlerp(a, b, t);\n"),

    # ---- ⚠️ shortest_path：取反是同一个旋转，"看朝向"分不出来 ----------------
    ("QE09 SlerpUnclamped turns shortest_path on", SCRIPTVEC_CPP,
     "::Quaternion::SLerp(t, a, b, /*shortest_path=*/false);",
     "::Quaternion::SLerp(t, a, b, /*shortest_path=*/true);"),

    ("QE10 LerpUnclamped turns shortest_path on", SCRIPTVEC_CPP,
     "::Quaternion::NLerp(t, a, b, /*shortest_path=*/false);",
     "::Quaternion::NLerp(t, a, b, /*shortest_path=*/true);"),

    # ---- 剩下的门面 ----------------------------------------------------------
    ("QE11 Mul(a,b) applies a first (operand order swapped)", SCRIPTVEC_CPP,
     "        return QuaternionMath::Mul(a, b);\n    }\n\n"
     "    ::Quaternion Quaternion::Nlerp(",
     "        return QuaternionMath::Mul(b, a);\n    }\n\n"
     "    ::Quaternion Quaternion::Nlerp("),

    # Normalize 什么都不做 ⇒ 零四元数原样返回（NaN 那条更隐蔽，形状一样）。
    ("QE12 Normalize stops normalizing", SCRIPTVEC_CPP,
     "        return QuaternionMath::Normalize(q);",
     "        return q;"),

    # 脚本面 Euler 把 x / z 对调：C++ 侧 QT13 红，TS 侧 U1 也红。
    ("QE13 the script face Euler swaps x and z", SCRIPTVEC_CPP, SCRIPT_EULER_SIG,
     "        return ::Quaternion::FromEulerAngles(::Vector3(x, y, z));",
     "        return ::Quaternion::FromEulerAngles(::Vector3(z, y, x));",
     SCRIPT_EULER_END),

    # ---- 门本身的门：让扫描"什么也没扫到" ------------------------------------
    ("QE14 the QW1 anchor is misspelled (the euler scan silently finds nothing)", PARITY_CPP,
     'transform_source.find("Quaternion EulerToQuaternion")',
     'transform_source.find("Quaternion EulerToQuaternionZZZ")'),

    ("QE15 the QW6 anchor is misspelled (the script-face scan finds nothing)", PARITY_CPP,
     'script_source.find("::Quaternion Quaternion::Euler(")',
     'script_source.find("::Quaternion Quaternion::EulerZZZ(")'),

    ("QE16 the QW9 anchor is misspelled (the facade-declaration scan finds nothing)",
     PARITY_CPP,
     'CountOccurrences(facade_header, "static ::Quaternion EulerVector(") >= 1',
     'CountOccurrences(facade_header, "static ::Quaternion EulerVectorZZZ(") >= 1'),

    # ---- ⚠️ 反方向退回 Ogre 遗产（Y-up）：没有 QW12/QW13 就完全静默 ----------
    ("QE17 EulerFromQuaternion falls back to the Y-up GetYaw", TRANSFORM_CPP,
     EULER_FROM_QUAT_SIG,
     "        return Quaternion::ToEulerAngles(rotation);",
     "        return Vector3(Deg(rotation.GetYaw().valueRadians()), 0.0f, 0.0f);",
     EULER_FROM_QUAT_END),

    # ---- TS 侧：往返的判据本身 ------------------------------------------------
    # ⚠️ 只改 Mover.js（编译产物）不改 Mover.ts：这条证明 U1 读的是**真跑出来的
    # JS 输出**，而不是硬编码。改完不需要重新构建，ScriptComponent 直接红。
    ("QE18 the TS probe feeds Euler its arguments in the wrong order", MOVER_JS,
     "const q = Quaternion.Euler(20, 30, 40);",
     "const q = Quaternion.Euler(40, 30, 20);"),

    # 90° 那条：绕 X 换成绕 Z，落点从 -Y 变成 +Y。
    ("QE19 the TS probe rotates about the wrong axis", MOVER_JS,
     "const r = Quaternion.Rotate(Quaternion.Euler(90, 0, 0), Vector3.up);",
     "const r = Quaternion.Rotate(Quaternion.Euler(0, 0, 90), Vector3.up);"),
]

# =============================================================================
# 已知存活项（accepted survivors）
# =============================================================================
# 变异测试的规则是"没被杀掉的变异体 = 要么缺测试、要么那段代码是多余的"。
# 下面这条属于后者，但**不是**该删的代码，所以单列出来而不是混进上面的表：
#
# QE02  FromEulerAngles 末尾的 `result.normalise()` 是防御性的。qx/qy/qz 都来自
#       `FromAngleAxis`（单位），三个单位四元数相乘在数学上仍是单位四元数，
#       float 下的实测漂移 < 1e-7 —— 远小于 QT11 的 1e-4 容差，所以删掉它
#       **一条测试都不会红**（实测：18/19，只有它存活）。
#
#       处理方式不是"再加一条断言"（再紧的容差也只是把同一个结论换个说法，
#       该存活还是存活），而是把**结论**钉成 QT28：漂移必须 < 1e-6。哪天某个
#       改动让中间量不再是单位（比如 `FromAngleAxis` 不再归一化），QT28 会先红，
#       那时 `normalise()` 就从"防御"变成"必需"，这条也该从表里划掉。
KNOWN_SURVIVORS = [
    ("QE02 FromEulerAngles forgets to normalise", QUAT_CPP, FROM_EULER_SIG,
     "    result.normalise();\n", "", FROM_EULER_END),
]

LOCK = os.path.join(BAK, "zengine_mutate_qe.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_qe.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    # `py .mutate_qe.py KNOWN` 把上面那条已知存活项也真跑一遍，确认它**仍然**
    # 存活（以及存活的理由没变）。只在改了 FromEulerAngles / QT28 之后需要跑。
    table = KNOWN_SURVIVORS if "KNOWN" in only else MUTATIONS
    if table is KNOWN_SURVIVORS:
        only = set()
    results = []
    touched = set()
    try:
        for entry in table:
            if len(entry) == 6:
                name, path, func_sig, old, new, end_marker = entry
            else:
                name, path, old, new = entry
                func_sig = None
                end_marker = None
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".qebak")
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
                with io.open(os.path.join(ROOT, ".mutate_qe.log"), "a", encoding="utf-8") as lg:
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
        print("\n[known survivor, re-confirmed] QE02 -- see KNOWN_SURVIVORS")
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
