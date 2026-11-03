"""Mutation harness for the ConfigurableJoint slice (#58).

What is under test
------------------
`PhysicsScene` grew the six-DOF joint: `configurable` in `PhysicsJointType`, the
per-axis motion / limit / drive accessors, the four target values, the linear
limit spring, and the `SixDOFConstraint` creation branch.
`ConfigurableJointComponent` grew on top, and `ConfigurableJointMotion` (ZENUM) +
`JointDrive` + the `ConfigurableJoint` alias went into the script surface.

Why these particular mutants
----------------------------
* **"Locked" is an inverted range in Jolt.** `MakeFixedAxis` writes
  `min = FLT_MAX / max = -FLT_MAX` and `IsFixedAxis` is `min >= max`. CFM05/CFM07
  make the setter accept an inverted range or report Locked as Limited; CF3a /
  CF4a / CF4b exist for exactly that.
* **The per-axis limit write must be a read-modify-write.** Jolt's
  `SetTranslationLimits` / `SetRotationLimits` take a whole `Vec3`, so writing one
  axis without reading the other two back silently re-limits them. CFM06 does
  that; CF3b is the gate. (A limit that got *wider* is only visible in the
  simulation, never in an error.)
* **Pyramid, not Cone.** Jolt's default swing type forces Y and Z to be symmetric
  and keeps only the max — Unity's `angularYLimit` / `angularZLimit` are
  independent. CFM12 flips it; CF27b is the gate, and it only discriminates
  because the fixture gives Y and Z *different* values.
* **The motor default is "off" but the spring settings are not.** Jolt's
  `MotorSettings` starts at `{FrequencyAndDamping, 2 Hz, 1}`, which reads back as
  a spring nobody authored. CFM08 drops the zeroing; CF6 is the gate.
* **"Is the motor on" is only observable behaviourally.** `GetJointAxisDrive`
  deliberately reports the spring settings even when the motor is off, so CFM09
  (never turn it on) can only be caught by watching a body move — CF11a.
* **targetRotation crosses two coordinate spaces.** It is written in body space
  and stored in constraint space. CFM10 drops the inverse; CF7c only
  discriminates because that fixture's frame axes are *not* the identity.

Run:  py .mutate_config.py            (all)
      py .mutate_config.py CFM01      (subset)
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
CFG_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/ConfigurableJointComponent.cpp")
JOINT_H = S(r"engine/Source/Runtime/Function/Physics/PhysicsJoint.h")
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

# 判一个**后端 / 组件**变异体只需要这两个。另外两个（各要再 link 一遍 ZRuntime）
# 只在变异体碰得到脚本发布面时才编 —— 见 build_and_run 里的 NEED_SCRIPT。
BASE_TARGETS = ("ZRuntime", "UnityParitySmokeTest")

DTS_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptTsChainSmokeTest.exe"
DTS_PATHS = (
    r"engine\EditorScripts\typings\zengine.d.ts",
    r"engine\Intermediate\Typings\zengine.d.ts",
    r"engine\Source\Runtime\Scripting\ScriptVM\Test\Typings\zengine.d.ts",
)

NEED_RELINK = False
# 这一轮变异体有没有可能改到 d.ts 的内容。只有碰 `ScriptBinding.cpp` / 枚举声明
# 的那几个才会，其余（后端实现、组件实现）根本不影响发布面。
NEED_DTS = True
# 同上，但是管"要不要编那两个脚本 exe"。与 NEED_DTS 同条件（能改 d.ts 的变异体
# 才可能改变 ScriptComponentSmokeTest 的结果）。
NEED_SCRIPT = True


def force_relink():
    """清掉三个 exe 的增量链接状态文件（.ilk）。

    Debug 默认开 /INCREMENTAL，而增量链接不搬改过的**大只读字符串**
    （`kZEngineDtsTemplate`，89KB）：.obj / .lib 里是新的、exe 里还是旧的。
    现在这三个 target 在 CMake 里挂了 `$<$<CONFIG:Debug>:/INCREMENTAL:NO>`，
    这里就是个 no-op，留着只为清旧 build 目录的残留。只删 .ilk，用 os.remove
    直接删，不进回收站。
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
    """刷 d.ts：只跑一遍 `--dump-dts` 再 copy 两份（原先是起三遍 QuickJS）。"""
    first = DTS_PATHS[0]
    r = subprocess.run([os.path.join(ROOT, DTS_EXE), "--dump-dts", os.path.join(ROOT, first)],
                       cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        print("  DTS REGEN FAILED rc=%d %s" % (r.returncode, (r.stdout or "")[:120]))
        return False
    for rel in DTS_PATHS[1:]:
        shutil.copy2(os.path.join(ROOT, first), os.path.join(ROOT, rel))
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


def _cmake_build(targets, parallel):
    argv = ["cmake", "--build", ".", "--target"] + list(targets) + ["--config", "Debug"]
    if parallel:
        # 用 CMake 自己的开关，不要手写 `-- /m`：Git Bash 会把 `/m` 转成路径，
        # MSBuild 直接 MSB1008。
        argv.append("--parallel")
    return subprocess.run(argv, cwd=os.path.join(ROOT, "build"),
                          env=ENV, capture_output=True, text=True, timeout=1800)


# ⚠️ 构建失败分两类，混在一起重试就是白烧时间：
#   * 真编译/链接错误 —— 确定的，重试多少次都一样。CFM02 一个编不过的变异体
#     按老逻辑烧了 12 次 cmake --build（并行重试 + 逐个 target 串行兜底 ×3 轮）。
#   * 文件锁 / 拷贝竞态 —— 重试能过，这才是串行兜底该出手的地方。
FLAKY = ("LNK1168", "LNK1104", "LNK1201",          # 文件被占 / 打不开
         "MSB3021", "MSB3026", "MSB3027",           # copy 失败
         "MSB3491", "MSB3721",
         "The process cannot access the file")
HARD_ERROR = (": error C", ": error LNK", ": error MSB", "fatal error C")


def _out(p):
    return (p.stdout or "") + (p.stderr or "")


def _classify(p):
    """'flaky' = 重试有意义；'hard' = 编译/链接错，再试也是同一个错。"""
    out = _out(p)
    if any(s in out for s in FLAKY):
        return "flaky"
    if any(s in out for s in HARD_ERROR):
        return "hard"
    return "unknown"


def build(refresh_dts=True, targets=None):
    """一次 `cmake --build` 编完 target，并行。

    只在**值得**的时候重试：编译错直接放弃，文件锁/拷贝竞态才走串行兜底。
    """
    targets = tuple(targets) if targets else TARGETS
    last = None
    if NEED_RELINK:
        force_relink()
    for attempt in range(2):
        p = _cmake_build(targets, parallel=True)
        last = p
        if p.returncode == 0:
            if refresh_dts and not regen_dts():
                return last
            return last
        if "LNK1236" in _out(p):
            _cmake_build(("ZRuntime",), parallel=True)
        kind = _classify(p)
        if kind == "hard":
            # 编不过就是编不过 —— 这个变异体判 "invalid (does not build)"。
            return last
        if attempt == 0:
            continue
        if kind == "unknown":
            # 认不出来的失败也可能是偶发，保留一次串行兜底。
            pass
        print("  (build flaked -- falling back to sequential)")
        for target in targets:
            p = _cmake_build((target,), parallel=False)
            last = p
            if p.returncode != 0:
                break
        else:
            if refresh_dts and not regen_dts():
                return last
        return last
    return last


def build_and_run():
    # 只有可能改动脚本发布面（prelude / Converter / ZENUM）的变异体才需要那两个
    # 脚本 exe —— 它们要 link 一遍 ZRuntime，而后端/组件的变异体用
    # UnityParitySmokeTest 一个就能判。
    targets = TARGETS if NEED_SCRIPT else BASE_TARGETS
    p = build(refresh_dts=NEED_DTS, targets=targets)
    if p is None or p.returncode != 0:
        print("  BUILD FAILED rc=%d" % (p.returncode if p is not None else -1))
        return "BUILD FAILED"
    rc_parity = run(PARITY_EXE)
    rc_comp = run(COMP_EXE) if NEED_SCRIPT else 0
    if rc_parity == 0 and rc_comp == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # =========================================================================
    # 后端：类型 / 按类型拒绝
    # =========================================================================
    ("CFM01 GetJointType reports a configurable as a point",
     SCENE_CPP, "bool PhysicsScene::GetJointType(uint32_t joint_id, PhysicsJointType& out_type) const",
     "            out_type = PhysicsJointType::configurable;",
     "            out_type = PhysicsJointType::point;"),

    ("CFM02 CreateJoint builds a ball socket for a configurable",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            JPH::SixDOFConstraintSettings settings;",
     "            JPH::PointConstraintSettings settings;"),

    ("CFM03 SetJointAxisMotion drops the type check",
     SCENE_CPP, "bool PhysicsScene::SetJointAxisMotion(uint32_t joint_id,",
     "    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)",
     "    if (jph_constraint == nullptr)"),

    ("CFM04 SetJointAxisDrive drops the type check",
     SCENE_CPP, "bool PhysicsScene::SetJointAxisDrive(uint32_t joint_id, ConfigurableJointAxis axis, const JointDrive& drive)",
     "    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)",
     "    if (jph_constraint == nullptr)"),

    # =========================================================================
    # Locked = 反序区间；改一个轴不能动另外两个
    # =========================================================================
    ("CFM05 SetJointAxisMotion accepts an inverted Limited range (that spelling means Locked)",
     SCENE_CPP, "bool PhysicsScene::SetJointAxisMotion(uint32_t joint_id,",
     "    if (motion == ConfigurableJointMotion::Limited && limit_min > limit_max)",
     "    if (false)"),

    ("CFM06 the per-axis limit write skips the read-modify-write (it clobbers the other two axes)",
     SCENE_CPP, "bool PhysicsScene::SetJointAxisMotion(uint32_t joint_id,",
     "        JPH::Vec3 min = joint->GetTranslationLimitsMin();\n"
     "        JPH::Vec3 max = joint->GetTranslationLimitsMax();\n"
     "        min.SetComponent(component, new_min);\n"
     "        max.SetComponent(component, new_max);\n"
     "        joint->SetTranslationLimits(min, max);",
     "        joint->SetTranslationLimits(JPH::Vec3::sReplicate(new_min), JPH::Vec3::sReplicate(new_max));"),

    ("CFM07 GetJointAxisMotion reports a Locked axis as Limited",
     SCENE_CPP, "bool PhysicsScene::GetJointAxisMotion(uint32_t joint_id,",
     "    else if (joint->IsFixedAxis(jolt_axis))",
     "    else if (false)"),

    ("CFM08 CreateJoint stops zeroing the motor settings (Jolt's 2 Hz default reads back as a phantom spring)",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "                settings.mMotorSettings[i] =\n"
     "                    JPH::MotorSettings(JPH::ESpringMode::StiffnessAndDamping, 0.f, 0.f);",
     "                (void)i;"),

    # =========================================================================
    # 驱动 / 目标量
    # =========================================================================
    ("CFM09 the drive writes the spring settings but never turns the motor on",
     SCENE_CPP, "bool ApplyDriveToAxis(JPH::SixDOFConstraint* in_joint, ConfigurableJointAxis in_axis, const JointDrive& in_drive)",
     "        in_joint->SetMotorState(jolt_axis,\n"
     "                                in_drive.positionSpring > 0.f ? JPH::EMotorState::PositionAndVelocity\n"
     "                                                              : JPH::EMotorState::Off);",
     "        in_joint->SetMotorState(jolt_axis, JPH::EMotorState::Off);"),

    ("CFM10 GetJointTarget reports constraint space instead of body space",
     SCENE_CPP, "bool PhysicsScene::GetJointTarget(uint32_t joint_id,",
     "    out_target_rotation = toQuat(c_to_b1 * joint->GetTargetOrientationCS() * c_to_b2.Conjugated());",
     "    out_target_rotation = toQuat(joint->GetTargetOrientationCS());"),

    # =========================================================================
    # 参考系：Pyramid + 平行轴
    # =========================================================================
    ("CFM12 the swing type goes back to Cone (it forces Y and Z to share one radius)",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            settings.mSwingType = JPH::ESwingType::Pyramid;",
     "            settings.mSwingType = JPH::ESwingType::Cone;"),

    ("CFM13 the secondary axis is handed to Jolt raw (parallel to axis => NaN frame)",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            settings.mAxisY1 = PlaneAxisFor(axis_x1, in_desc.m_SecondaryAxisA);",
     "            settings.mAxisY1 = toVec3(in_desc.m_SecondaryAxisA);"),

    ("CFM14 CreateJoint accepts a desc with an inverted limit range",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "                        if (in_desc.m_AxisLimitMin[i] > in_desc.m_AxisLimitMax[i])",
     "                        if (false)"),

    # =========================================================================
    # ConfigurableJointComponent
    # =========================================================================
    ("CFM20 GetJointType reports a ConfigurableJoint as a point",
     CFG_CPP, "PhysicsJointType ConfigurableJointComponent::GetJointType() const",
     "    return PhysicsJointType::configurable;",
     "    return PhysicsJointType::point;"),

    ("CFM21 SetXMotion stops checking that the enum is in range",
     CFG_CPP, "bool ConfigurableJointComponent::SetXMotion(ConfigurableJointMotion motion)",
     "    if (!IsValidMotion(motion))",
     "    if (false)"),

    ("CFM22 SetSecondaryAxis forgets to rebuild (the frame is baked into the constraint)",
     CFG_CPP, "void ConfigurableJointComponent::SetSecondaryAxis(const Vector3& secondary_axis)",
     "    m_JointRes.m_SecondaryAxis = secondary_axis;\n"
     "    // 与 `JointComponent::SetAxis` 同理：参考系是构造时烘进约束的，改了只能重建。\n"
     "    RebuildJoint();",
     "    m_JointRes.m_SecondaryAxis = secondary_axis;"),

    ("CFM23 SetLinearLimit accepts a negative limit",
     CFG_CPP, "bool ConfigurableJointComponent::SetLinearLimit(float limit)",
     "    if (limit < 0.f)",
     "    if (false)"),

    ("CFM24 SetAngularXLimits accepts a range that misses zero",
     CFG_CPP, "bool ConfigurableJointComponent::SetAngularXLimits(float low, float high)",
     "    if (low > 0.f || high < 0.f)",
     "    if (false)"),

    ("CFM25 SetAngularYLimit accepts a negative half angle",
     CFG_CPP, "bool ConfigurableJointComponent::SetAngularYLimit(float angle)",
     "    if (angle < 0.f)",
     "    if (false)"),

    ("CFM26 SetAngularYZDrive only drives Y (Z keeps the old drive)",
     CFG_CPP, "bool ConfigurableJointComponent::SetAngularYZDrive(const JointDrive& drive)",
     "    if (!CommitDrive(ConfigurableJointAxis::RotationY, drive))\n"
     "    {\n"
     "        return false;\n"
     "    }\n"
     "    return CommitDrive(ConfigurableJointAxis::RotationZ, drive);",
     "    return CommitDrive(ConfigurableJointAxis::RotationY, drive);"),

    ("CFM27 PushLinearLimitSpring is a no-op (the backend never hears about the spring)",
     CFG_CPP, "void ConfigurableJointComponent::PushLinearLimitSpring()",
     "    physics_scene->SetJointLinearLimitSpring(m_JointId, m_JointRes.m_LinearLimitSpring, m_JointRes.m_LinearLimitDamper);",
     "    if (true) { (void)0; } else physics_scene->SetJointLinearLimitSpring(m_JointId, m_JointRes.m_LinearLimitSpring, m_JointRes.m_LinearLimitDamper);"),

    ("CFM28 PushTargets is a no-op (the backend never hears about the targets)",
     CFG_CPP, "void ConfigurableJointComponent::PushTargets()",
     "    physics_scene->SetJointTarget(m_JointId,",
     "    if (true) { (void)0; } else physics_scene->SetJointTarget(m_JointId,"),

    ("CFM29 CommitDrive stops validating the drive",
     CFG_CPP, "bool ConfigurableJointComponent::CommitDrive(ConfigurableJointAxis axis, const JointDrive& drive)",
     "    if (drive.positionSpring < 0.f || drive.positionDamper < 0.f || drive.maximumForce < 0.f)",
     "    if (false)"),

    # =========================================================================
    # 发布面 / 接线
    # =========================================================================
    ("CFM30 the JointDrive interface is renamed away",
     BINDING_CPP, None,
     '"declare interface JointDrive {\\n"',
     '"declare interface JointDriveRenamed {\\n"'),

    ("CFM31 the ConfigurableJoint alias disappears from the prelude",
     BINDING_CPP, None,
     '"declare type ConfigurableJoint = ConfigurableJointComponent;\\n"',
     '""'),

    ("CFM32 the marshaller for JointDrive is gone",
     BINDING_H, None,
     "    struct Converter<JointDrive>",
     "    struct Converter<JointDriveRenamed>"),

    ("CFM33 the ZENUM annotation on ConfigurableJointMotion is gone",
     JOINT_H, None,
     "ZENUM()\nenum class ConfigurableJointMotion : unsigned char",
     "enum class ConfigurableJointMotion : unsigned char"),

    ("CFM34 the no-physics build loses SetJointAxisMotion",
     NULL_CPP, None,
     "PhysicsScene::SetJointAxisMotion(uint32_t, ConfigurableJointAxis",
     "PhysicsScene::SetJointAxisMotionRenamed(uint32_t, ConfigurableJointAxis"),
]

LOCK = os.path.join(BAK, "zengine_mutate_config.lock")


def lock_is_stale():
    """锁文件存在**不代表真有人在跑** —— 上次被 SIGTERM 打断时锁就留下了。
    判据两条：PID 还在不在，以及锁的年龄（PID 可能被复用，所以要有时间兜底）。"""
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
            raise SystemExit("another .mutate_config.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    args = sys.argv[1:]
    # `--resume`：日志里已经有结论的变异体跳过。中途被打断（或者只想重跑存活的
    # 那几个）时不用从头再来 —— 一轮 28 个变异体，重跑一遍很贵。
    resume = "--resume" in args
    only = set(a for a in args if not a.startswith("--"))
    done = set()
    if resume:
        log = os.path.join(ROOT, ".mutate_config.log")
        if os.path.exists(log):
            for line in io.open(log, "r", encoding="utf-8"):
                parts = line.split()
                if parts:
                    done.add(parts[0])
            print("! --resume: %d mutants already have a verdict, skipping them" % len(done))
    results = []
    touched = set()

    # ⚠️ 内层的 `finally` 挡不住 SIGTERM / Ctrl-C：信号直接终止进程，不抛异常，
    # 于是**变异体就留在源码里了**。上一次这么留下的残留害得白改四轮夹具，所以
    # 这里显式接管信号，死之前一定把源码还原回去。
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
        # 基线：先把源码按**未变异**的状态编一遍并刷一次 d.ts。既然多数变异体现在
        # 都不刷 d.ts 了，这一遍就是整个 run 里唯一保证 d.ts 与当前源码一致的机会。
        print("=== baseline (clean sources) ===")
        b = build(refresh_dts=True)
        if b is None or b.returncode != 0:
            print("! baseline build FAILED -- results below are not trustworthy")
        print()

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
            if name.split()[0] in done:
                continue
            print("=== %s ===" % name)
            global NEED_RELINK, NEED_DTS, NEED_SCRIPT
            NEED_RELINK = path in (BINDING_CPP, BINDING_H)
            NEED_DTS = path in (BINDING_CPP, BINDING_H, JOINT_H)
            NEED_SCRIPT = NEED_DTS
            bak = os.path.join(BAK, os.path.basename(path) + ".cfgbak")
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
                with io.open(os.path.join(ROOT, ".mutate_config.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        build(refresh_dts=True)      # 收尾：源码已还原，重建 + 刷回正确的 d.ts
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
