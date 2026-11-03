"""Mutation harness for the CharacterJoint slice (#57).

What is under test
------------------
`PhysicsScene` grew the swing-twist (cone) joint: `swing_twist` in
`PhysicsJointType`, six new `PhysicsJointDesc` fields, four runtime accessors
(limits + springs), the impulse branch, and `PlaneAxisFor` (the anti-NaN guard
for a swing axis parallel to the twist axis). `CharacterJointComponent` grew on
top, and `SoftJointLimitSpring` + the `CharacterJoint` alias went into the
script surface.

Why these particular mutants
----------------------------
* **The cone is its own type, and the four limit accessors refuse by type.**
  CJM01/CJM03 make the type lie or drop the refusal; CJ0/CJ0b/CJ0c exist for
  exactly that (a raw backend joint, plus a hinge that must be refused).
* **"Refuse" must live in BOTH layers.** `CharacterJointComponent` validates
  before it stores, and `PhysicsScene` validates again. A programmatic ragdoll
  only goes through the backend one, so CJ0e drives the backend directly --
  without it the backend's checks could be deleted outright.
* **Validation happens BEFORE the store.** CJM12 stores first and validates
  after: the getter then reports a value the script never successfully set, with
  no error anywhere. CJ8/CJ9/CJ16 read the old value back to catch it.
* **A cone limit does NOT rebuild the joint -- unlike a hinge's.** CJ6 pins that
  down (CJ19 adds a rebuild; CJ12 pins the opposite for `swingAxis`, which IS
  baked into the constraint frame).
* **`PlaneAxisFor` exists because Jolt orthogonalizes once.** CJM09 hands Jolt
  the raw parallel axis: `cross(parallel, parallel)` is zero, `.Normalized()` is
  NaN, and one step later the whole scene is NaN. "CreateJoint returned an id"
  stays green through all of that -- which is why CJ0f checks the bodies are
  still finite after stepping.

Run:  py .mutate_cjoint.py            (all)
      py .mutate_cjoint.py CJM01      (subset)
"""
import io
import os
import shutil
import signal
import subprocess
import sys
import time

# path -> backup，供信号处理器在中途被杀时还原（见 main 里的注释）。
RESTORE = {}

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


SCENE_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsScene.cpp")
NULL_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsSceneNull.cpp")
CHAR_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/CharacterJointComponent.cpp")
BINDING_CPP = S(r"engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp")
BINDING_H = S(r"engine/Source/Runtime/Scripting/Binding/ScriptBinding.h")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n}\n"
END_OF_CLASS_FUNC = "\n    }\n"


def marker_for(path):
    return END_OF_CLASS_FUNC if path.endswith((".h",)) else END_OF_FUNC


TARGETS = ("ZRuntime", "ScriptTsChainSmokeTest", "UnityParitySmokeTest", "ScriptComponentSmokeTest")

DTS_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptTsChainSmokeTest.exe"
DTS_PATHS = (
    r"engine\EditorScripts\typings\zengine.d.ts",
    r"engine\Intermediate\Typings\zengine.d.ts",
    r"engine\Source\Runtime\Scripting\ScriptVM\Test\Typings\zengine.d.ts",
)

NEED_RELINK = False


def force_relink():
    """兜底：清掉这三个 exe 的增量链接状态文件（.ilk）。

    Debug 默认开 /INCREMENTAL，而增量链接不搬改过的**大只读字符串**
    （`kZEngineDtsTemplate`，89KB 拼接字面量）：.obj / .lib 里是新的、exe 里
    还是旧的，d.ts 门禁于是读到上一次的字符串 —— 编译零警告、测试零失败、
    变异体假活。

    现在这三个 target 在 CMake 里已经挂了 `$<$<CONFIG:Debug>:/INCREMENTAL:NO>`，
    新构建根本不产生 .ilk，这里就是个 no-op。留着只是为了清掉**旧 build 目录**
    里已经存在的 .ilk（换过 CMake 之前留下的）。

    只删 .ilk 这一个文件，用 os.remove 直接删，不进回收站、不动构建产物。
    """
    for rel in (
        r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptTsChainSmokeTest.ilk",
        r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.ilk",
        r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.ilk",
    ):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass


def regen_dts():
    for rel in DTS_PATHS:
        r = subprocess.run([os.path.join(ROOT, DTS_EXE), "--dump-dts", os.path.join(ROOT, rel)],
                           cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
        if r.returncode != 0:
            print("  DTS REGEN FAILED rc=%d %s" % (r.returncode, (r.stdout or "")[:120]))
            return False
    return True


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    if old == new:
        raise SystemExit("NO-OP MUTANT in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else marker_for(path)
    j = text.find(marker, i)
    if j < 0:
        raise SystemExit("END OF FUNC NOT FOUND: %r" % func_sig)
    region = text[i:j]
    if old not in region:
        raise SystemExit("PATTERN NOT FOUND in %s [%s]:\n%r" % (path, func_sig, old))
    if old == new:
        raise SystemExit("NO-OP MUTANT in %s:\n%r" % (path, old))
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
    if NEED_RELINK:
        force_relink()
    for _ in range(3):
        ok = True
        for target in TARGETS:
            p = subprocess.run(["cmake", "--build", ".", "--target", target,
                                "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                               env=ENV, capture_output=True, text=True, timeout=1800)
            last = p
            if p.returncode != 0:
                ok = False
                if "LNK1236" in (p.stdout or ""):
                    subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime",
                                    "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                                   env=ENV, capture_output=True, text=True, timeout=1800)
                break
        if ok:
            if not regen_dts():
                return last
            return last
    return last


def build_and_run():
    p = build()
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        return "BUILD FAILED"
    if not regen_dts():
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # =========================================================================
    # 后端：类型 / 按类型拒绝
    # =========================================================================
    ("CJM01 GetJointType reports a swing-twist as a point",
     SCENE_CPP, "bool PhysicsScene::GetJointType(uint32_t joint_id, PhysicsJointType& out_type) const",
     "            out_type = PhysicsJointType::swing_twist;",
     "            out_type = PhysicsJointType::point;"),

    ("CJM02 CreateJoint builds a ball socket for a cone",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            JPH::SwingTwistConstraintSettings settings;",
     "            JPH::PointConstraintSettings settings;"),

    ("CJM03 SetJointSwingTwistLimits drops the type check",
     SCENE_CPP, "bool PhysicsScene::SetJointSwingTwistLimits(uint32_t joint_id,",
     "    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SwingTwist)",
     "    if (jph_constraint == nullptr)"),

    ("CJM04 the backend stops refusing a negative half cone angle",
     SCENE_CPP, "bool PhysicsScene::SetJointSwingTwistLimits(uint32_t joint_id,",
     "    if (swing1_half_angle < 0.f || swing2_half_angle < 0.f)",
     "    if (false)"),

    ("CJM05 the backend stops refusing a twist range that misses zero",
     SCENE_CPP, "bool PhysicsScene::SetJointSwingTwistLimits(uint32_t joint_id,",
     "    if (twist_min_angle > 0.f || twist_max_angle < 0.f)",
     "    if (false)"),

    ("CJM06 SetJointSwingTwistLimits swaps the two swing half angles",
     SCENE_CPP, "bool PhysicsScene::SetJointSwingTwistLimits(uint32_t joint_id,",
     "    joint->SetNormalHalfConeAngle(swing1_half_angle);\n"
     "    joint->SetPlaneHalfConeAngle(swing2_half_angle);",
     "    joint->SetNormalHalfConeAngle(swing2_half_angle);\n"
     "    joint->SetPlaneHalfConeAngle(swing1_half_angle);"),

    ("CJM07 GetJointSwingTwistLimits swaps twist min and max",
     SCENE_CPP, "bool PhysicsScene::GetJointSwingTwistLimits(uint32_t joint_id,",
     "    out_twist_min_angle = joint->GetTwistMinAngle();",
     "    out_twist_min_angle = joint->GetTwistMaxAngle();"),

    ("CJM08 SetJointSwingTwistSpring swaps swing and twist",
     SCENE_CPP, "bool PhysicsScene::SetJointSwingTwistSpring(uint32_t joint_id,",
     "    joint->GetSwingMotorSettings().mSpringSettings = JPH::SpringSettings(\n"
     "        JPH::ESpringMode::FrequencyAndDamping, swing_frequency_hz, swing_damping);",
     "    joint->GetSwingMotorSettings().mSpringSettings = JPH::SpringSettings(\n"
     "        JPH::ESpringMode::FrequencyAndDamping, twist_frequency_hz, twist_damping);"),

    ("CJM09 GetJointSwingTwistSpring reports the swing spring as the twist one",
     SCENE_CPP, "bool PhysicsScene::GetJointSwingTwistSpring(uint32_t joint_id,",
     "    out_twist_frequency_hz = joint->GetTwistMotorSettings().mSpringSettings.mFrequency;",
     "    out_twist_frequency_hz = joint->GetSwingMotorSettings().mSpringSettings.mFrequency;"),

    ("CJM10 the cone impulse is always zero (breakForce could never fire)",
     SCENE_CPP, "bool PhysicsScene::GetJointImpulse(uint32_t joint_id,",
     "            out_angular_impulse = std::max(std::abs(joint->GetTotalLambdaTwist()),",
     "            out_angular_impulse = std::max(0.f,"),

    # =========================================================================
    # 退化扭转区间（0..0）= 锁死，与 slice 56 铰链那条是同一条规律。
    # 守卫写成 `< 0 / > 0` 的话 0..0 会被当成"没配"丢掉 ⇒ Jolt 走 TwistXFree，
    # Unity 里"CharacterJoint 不许扭"就悄悄变成"随便扭"。
    # =========================================================================
    ("CJM23 the cone drops the degenerate twist range (0..0 stops locking the twist)",
     SCENE_CPP, "        case PhysicsJointType::swing_twist:",
     "            if (in_desc.m_TwistMinAngle <= 0.f && in_desc.m_TwistMaxAngle >= 0.f)",
     "            if (in_desc.m_TwistMinAngle < 0.f && in_desc.m_TwistMaxAngle > 0.f)"),

    ("CJM25 SetJointSwingTwistLimits refuses the degenerate 0..0 range (it must mean locked)",
     SCENE_CPP, "bool PhysicsScene::SetJointSwingTwistLimits(uint32_t joint_id,",
     "    if (twist_min_angle > 0.f || twist_max_angle < 0.f)",
     "    if (twist_min_angle >= 0.f || twist_max_angle <= 0.f)"),

    # =========================================================================
    # PlaneAxisFor：平行的摆动轴 = NaN
    # =========================================================================
    ("CJM11 PlaneAxisFor hands Jolt the raw parallel axis (NaN one step later)",
     SCENE_CPP, "JPH::Vec3 PlaneAxisFor(const JPH::Vec3& in_twist_axis, const Vector3& in_requested)",
     "        JPH::Vec3 orthogonal = requested - in_twist_axis * along;",
     "        JPH::Vec3 orthogonal = requested;"),

    # =========================================================================
    # CharacterJointComponent
    # =========================================================================
    ("CJM12 GetJointType reports a CharacterJoint as a point",
     CHAR_CPP, "PhysicsJointType CharacterJointComponent::GetJointType() const",
     "    return PhysicsJointType::swing_twist;",
     "    return PhysicsJointType::point;"),

    ("CJM13 SetSwing1Limit clamps a negative angle instead of refusing",
     CHAR_CPP, "bool CharacterJointComponent::SetTwistSwingLimits(float swing1, float swing2,",
     "    if (swing1 < 0.f || swing2 < 0.f)",
     "    if ((swing1 = swing1 < 0.f ? 0.f : swing1) < 0.f || (swing2 = swing2 < 0.f ? 0.f : swing2) < 0.f)"),

    ("CJM14 SetTwistSwingLimits stores first and validates after",
     CHAR_CPP, "bool CharacterJointComponent::SetTwistSwingLimits(float swing1, float swing2,",
     "    if (swing1 < 0.f || swing2 < 0.f)\n"
     "    {\n"
     "        LOG_ERROR(ZPhysics, \"CharacterJoint: swing half cone angles must be >= 0 ({} / {})\", swing1, swing2);\n"
     "        return false;\n"
     "    }",
     "    m_JointRes.m_Swing1Limit = swing1;\n"
     "    m_JointRes.m_Swing2Limit = swing2;"),

    ("CJM15 SetSwingAxis forgets to rebuild (the axis is baked into the frame)",
     CHAR_CPP, "void CharacterJointComponent::SetSwingAxis(const Vector3& swing_axis)",
     "    m_JointRes.m_SwingAxis = swing_axis;\n"
     "    // 与 `JointComponent::SetAxis` 同理：轴烘在约束的参考系里，改了只能重建。\n"
     "    RebuildJoint();",
     "    m_JointRes.m_SwingAxis = swing_axis;"),

    ("CJM16 PushLimits is a no-op (the backend never hears about the limits)",
     CHAR_CPP, "void CharacterJointComponent::PushLimits()",
     "    physics_scene->SetJointSwingTwistLimits(m_JointId,",
     "    if (true) { (void)0; } else physics_scene->SetJointSwingTwistLimits(m_JointId,"),

    ("CJM17 SetSwingLimitSpring accepts a negative spring",
     CHAR_CPP, "bool CharacterJointComponent::SetSwingLimitSpring(const SoftJointLimitSpring& spring)",
     "    if (spring.spring < 0.f || spring.damper < 0.f)",
     "    if (false)"),

    ("CJM19 changing a cone limit rebuilds the joint (the hinge's rule, wrongly)",
     CHAR_CPP, "bool CharacterJointComponent::SetTwistSwingLimits(float swing1, float swing2,",
     "    PushLimits();\n    return true;",
     "    PushLimits();\n    RebuildJoint();\n    return true;"),

    # =========================================================================
    # 发布面 / 接线
    # =========================================================================
    ("CJM18 the CharacterJoint alias disappears from the prelude",
     BINDING_CPP, None,
     '"declare type CharacterJoint = CharacterJointComponent;\\n"',
     '""'),

    ("CJM20 the SoftJointLimitSpring interface is renamed away",
     BINDING_CPP, None,
     '"declare interface SoftJointLimitSpring {\\n"',
     '"declare interface SoftJointLimitSpringRenamed {\\n"'),

    ("CJM21 the marshaller for SoftJointLimitSpring is gone",
     BINDING_H, None,
     "    struct Converter<SoftJointLimitSpring>",
     "    struct Converter<SoftJointLimitSpringRenamed>"),

    ("CJM22 the no-physics build loses SetJointSwingTwistLimits",
     NULL_CPP, None,
     "bool PhysicsScene::SetJointSwingTwistLimits(uint32_t, float, float, float, float)",
     "bool PhysicsScene::SetJointSwingTwistLimitsRenamed(uint32_t, float, float, float, float)"),
]

LOCK = os.path.join(BAK, "zengine_mutate_cjoint.lock")


def lock_is_stale():
    """锁文件存在**不代表真有人在跑** —— 上次被 SIGTERM 打断时锁就留下了，于是
    下一次运行直接被自己的残留挡在门外。判据两条：PID 还在不在，以及锁的年龄
    （PID 可能被复用，所以还要有时间兜底）。"""
    try:
        with io.open(LOCK, "r", encoding="utf-8") as f:
            pid = int((f.read().strip() or "0"))
    except (OSError, ValueError):
        return True
    if pid <= 0:
        return True
    try:
        os.kill(pid, 0)          # 存在性探测，不真的发信号
    except OSError:
        return True              # 进程已经没了
    return (time.time() - os.path.getmtime(LOCK)) > 6 * 3600


def main():
    if os.path.exists(LOCK):
        if lock_is_stale():
            print("! removing a stale lock (%s) -- a previous run was killed" % LOCK)
            os.remove(LOCK)
        else:
            raise SystemExit("another .mutate_cjoint.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()

    # ⚠️ 内层的 `finally` 挡不住 SIGTERM / Ctrl-C：信号直接终止进程，不抛异常，
    # 于是**变异体就留在源码里了**。那一次的后果极其昂贵 —— `out_angular_impulse
    # = std::max(0.f, ...)` 在仓库里躺了好几轮，之后所有"角冲量恒为 0"的观察
    # 全是这个残留造成的假象，白白改了四轮夹具和文档。所以这里显式接管信号，
    # 死之前一定把源码还原回去。
    def _restore_and_die(signum, _frame):
        sys.stderr.write("\n!! signal %d -- restoring sources before exit\n" % signum)
        for path, bak in RESTORE.items():
            try:
                shutil.copy2(bak, path)
                os.utime(path, None)
            except OSError:
                pass
        try:
            os.remove(LOCK)
        except OSError:
            pass
        sys.exit(1)

    signal.signal(signal.SIGTERM, _restore_and_die)
    signal.signal(signal.SIGINT, _restore_and_die)

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
            global NEED_RELINK
            NEED_RELINK = path in (BINDING_CPP, BINDING_H)
            bak = os.path.join(BAK, os.path.basename(path) + ".cjointbak")
            shutil.copy2(path, bak)
            RESTORE[path] = bak
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
                with io.open(os.path.join(ROOT, ".mutate_cjoint.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        build()
        regen_dts()
        print("---- summary ----")
        for n, r in results:
            print("%-70s %s" % (n, r))
        killed = [n for n, r in results if r in ("caught", "invalid (does not build)")]
        bad = [n for n, r in results if n not in killed]
        print("\n%d/%d killed (%d caught by a test, %d do not build)" %
              (len(killed), len(results),
               len([n for n, r in results if r == "caught"]),
               len([n for n, r in results if r == "invalid (does not build)"])))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
