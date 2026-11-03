"""Mutation harness for the Joint completion slice (#56).

What is under test
------------------
`PhysicsScene` grew the joint runtime control (type / enable / angle / velocity /
limits / motors / distance / spring / impulse / world anchor) and
`Framework/Component/Joint/` grew Unity's `Joint` family:
`JointComponent` (abstract) + `HingeJointComponent` / `FixedJointComponent` /
`SpringJointComponent` / `SliderJointComponent`. `PhysicsJoint.h` grew the three
Unity value types and `ScriptBinding` grew their converters.

Why these particular mutants
----------------------------
* **The type of a joint is never stored, it is always asked of Jolt.** JN01/JN15
  make `GetJointType` lie about one subtype; they are caught only because the test
  asks the backend what it actually built for *each* component class (JN58/JN75/
  JN76/JN80). A side table would be a second source of truth and would go stale
  after `RemoveJoint` -- that is the bug this design avoids.
* **"Refuse by type" is the whole contract.** JN04 drops the hinge check from
  `GetJointAngle`; it is caught only by the control case JN12 (a fixed joint must
  be refused). Without that control the method could be `out = 0; return true;`.
* **`min > max` must be refused, not swapped.** JN06/JN09 mutate exactly that.
* **Limits are baked into the constraint** (`HingeConstraint::SetLimits` cannot
  turn them off), so `useLimits` has to rebuild. JN25 removes that rebuild; it is
  caught by JN62/JN63, which compare the joint id before and after and then ask
  the backend for the limits.
* **`connectedAnchor` is local to the connected body -- except when there is no
  connected body, when it is world space.** JN17 (drop the rebuild) and the
  JN88a/JN88b pair exist for this: on a fixture where both pivots coincide,
  "ignore connectedAnchor" and "honour it" give the same distance 0.
* **`SetEnabled` has to reach Jolt**, not just flip a field (JN19); and
  `FixedTick` must not break a joint whose threshold is infinity (JN20).

* **A locked joint (min == max) must still build.** Jolt asserts
  `mLimitsMin != mLimitsMax || frequency > 0` in both the hinge and the slider
  constructor -- "better use a fixed constraint" -- but Unity's
  `useLimits = true` before a range is chosen means exactly that input. JN37/JN38
  put the frequency back to 0 and are caught because `AssertFailed` is a counted
  failure in this test binary; JN39 restores the old strict `<`/`>` guard, which
  turns "locked" into "no limits at all" -- the silent no-op kind of bug.

Deliberately NOT mutated (equivalent mutants):
  * `SetJointSpring` becoming a no-op: nothing in the test reads the spring
    settings back through a getter, so it would survive.
  * `SetJointPositionMotor`'s motor state: Jolt has no getter for it in the test's
    reach, so Velocity vs Position is indistinguishable here.
  * `PushMotor` ignoring `useSpring`: the assertions only check that the fields
    round-trip and that the joint was not rebuilt, both of which stay true.

Run:  py .mutate_joints.py            (all)
      py .mutate_joints.py JN01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


SCENE_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsScene.cpp")
NULL_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsSceneNull.cpp")
JOINT_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/JointComponent.cpp")
HINGE_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/HingeJointComponent.cpp")
SPRING_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/SpringJointComponent.cpp")
SLIDER_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Joint/SliderJointComponent.cpp")
WORLD_CPP = S(r"engine/Source/Runtime/Function/Framework/World/WorldManager.cpp")
BINDING_CPP = S(r"engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp")
BINDING_H = S(r"engine/Source/Runtime/Scripting/Binding/ScriptBinding.h")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# .cpp 里的目标函数在文件作用域，函数收尾是顶格的 `}`。
END_OF_FUNC = "\n}\n"
# ⚠️ 类里的内联函数：函数体 8 空格，收尾是 4 空格的 `}`。
END_OF_CLASS_FUNC = "\n    }\n"


def marker_for(path):
    return END_OF_CLASS_FUNC if path.endswith((".h",)) else END_OF_FUNC


TARGETS = ("ZRuntime", "ScriptTsChainSmokeTest", "UnityParitySmokeTest", "ScriptComponentSmokeTest")

# ⚠️ `zengine.d.ts` 是**生成物**，只在这里重新生成：
#   ScriptTsChainSmokeTest --dump-dts <path>
# 测试里那几条 `declare class / declare type / declare interface` 接线门读的是
# 签进仓库（其实不入 git）的那份 d.ts。不重新生成的话，"把 prelude 里的别名/
# interface 删掉"这类改动在测试里**根本看不见** —— 变异体 JN31/JN32 就是这么
# 活下来的。所以每次 build 之后都要刷新这三处。
DTS_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptTsChainSmokeTest.exe"
DTS_PATHS = (
    r"engine\EditorScripts\typings\zengine.d.ts",
    r"engine\Intermediate\Typings\zengine.d.ts",
    r"engine\Source\Runtime\Scripting\ScriptVM\Test\Typings\zengine.d.ts",
)


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


def force_relink():
    """⚠️ Debug 用的是 /INCREMENTAL（.ilk）。改一个大只读字符串（`kZEngineDtsTemplate`
    那种）时，增量链接**不重新搬数据**：新的 .obj 里有那一行，链接出来的 exe 里
    没有，于是"把 d.ts 里的别名删掉"这种变异体看起来活着、其实根本没进二进制。
    每次 build 前把测试 exe 的 .ilk 删掉，强制整链。"""
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


NEED_RELINK = False


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
    # PhysicsScene：类型 / 启用
    # =========================================================================
    ("JN01 GetJointType reports a hinge as fixed",
     SCENE_CPP, "bool PhysicsScene::GetJointType(uint32_t joint_id, PhysicsJointType& out_type) const",
     "            out_type = PhysicsJointType::hinge;",
     "            out_type = PhysicsJointType::fixed;"),

    ("JN15 GetJointType reports a slider as fixed",
     SCENE_CPP, "bool PhysicsScene::GetJointType(uint32_t joint_id, PhysicsJointType& out_type) const",
     "            out_type = PhysicsJointType::slider;",
     "            out_type = PhysicsJointType::fixed;"),

    ("JN02 SetJointEnabled is a no-op",
     SCENE_CPP, "bool PhysicsScene::SetJointEnabled(uint32_t joint_id, bool enabled)",
     "    jph_constraint->SetEnabled(enabled);",
     "    (void)enabled;"),

    ("JN03 IsJointEnabled always reports enabled",
     SCENE_CPP, "bool PhysicsScene::IsJointEnabled(uint32_t joint_id, bool& out_enabled) const",
     "    out_enabled = jph_constraint->GetEnabled();",
     "    out_enabled = true;"),

    # =========================================================================
    # PhysicsScene：角度 / 角速度
    # =========================================================================
    ("JN04 GetJointAngle drops the type check (any constraint answers)",
     SCENE_CPP, "bool PhysicsScene::GetJointAngle(uint32_t joint_id, float& out_angle_radians) const",
     "if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::Hinge)",
     "if (jph_constraint == nullptr)"),

    ("JN05 GetJointVelocity always reports zero",
     SCENE_CPP, "bool PhysicsScene::GetJointVelocity(uint32_t joint_id, float& out_radians_per_second) const",
     "    out_radians_per_second = relative_angular.Dot(world_axis);",
     "    out_radians_per_second = 0.f;"),

    # =========================================================================
    # PhysicsScene：限位 / 距离 / 冲量
    # =========================================================================
    ("JN06 SetJointLimits accepts min > max instead of refusing",
     SCENE_CPP, "bool PhysicsScene::SetJointLimits(uint32_t joint_id, float min_limit, float max_limit)",
     "    if (min_limit > max_limit)",
     "    if (false)"),

    ("JN07 SetJointLimits silently ignores the slider case",
     SCENE_CPP, "bool PhysicsScene::SetJointLimits(uint32_t joint_id, float min_limit, float max_limit)",
     "            static_cast<JPH::SliderConstraint*>(jph_constraint)->SetLimits(min_limit, max_limit);",
     "            static_cast<JPH::SliderConstraint*>(jph_constraint)->SetLimits(0.f, 0.f);"),

    ("JN13 GetJointLimits reports a constant for the hinge",
     SCENE_CPP, "bool PhysicsScene::GetJointLimits(uint32_t joint_id, float& out_min_limit, float& out_max_limit) const",
     "            out_min_limit = hinge->GetLimitsMin();",
     "            out_min_limit = 0.f;"),

    ("JN09 SetJointDistance swaps min and max",
     SCENE_CPP, "bool PhysicsScene::SetJointDistance(uint32_t joint_id, float min_distance, float max_distance)",
     "static_cast<JPH::DistanceConstraint*>(jph_constraint)->SetDistance(min_distance, max_distance);",
     "static_cast<JPH::DistanceConstraint*>(jph_constraint)->SetDistance(max_distance, min_distance);"),

    ("JN36 GetJointDistance swaps min and max",
     SCENE_CPP, "bool PhysicsScene::GetJointDistance(uint32_t joint_id, float& out_min_distance, float& out_max_distance) const",
     "    out_min_distance = distance_constraint->GetMinDistance();",
     "    out_min_distance = distance_constraint->GetMaxDistance();"),

    ("JN10 GetJointImpulse always reports zero (breakForce could never fire)",
     SCENE_CPP, "bool PhysicsScene::GetJointImpulse(uint32_t joint_id, float& out_linear_impulse, float& out_angular_impulse) const",
     "            out_linear_impulse = fixed->GetTotalLambdaPosition().Length();",
     "            out_linear_impulse = 0.f;"),

    # =========================================================================
    # PhysicsScene：世界锚点 / 拆除
    # =========================================================================
    ("JN11 CreateStaticAnchorBody puts the anchor at the origin",
     SCENE_CPP, "uint32_t PhysicsScene::CreateStaticAnchorBody(const Vector3& world_position)",
     "                                           JPH::RVec3(toVec3(world_position)),",
     "                                           JPH::RVec3::sZero(),"),

    ("JN12 RemoveJoint leaves the slot filled (the id stays alive)",
     SCENE_CPP, "bool PhysicsScene::RemoveJoint(uint32_t joint_id)",
     "    m_Joints[joint_id - 1] = nullptr;",
     "    ;"),

    # =========================================================================
    # JointComponent
    # =========================================================================
    ("JN16 RebuildJoint forgets to destroy the old joint",
     JOINT_CPP, "bool JointComponent::RebuildJoint()",
     "    DestroyJoint();",
     "    ;"),

    ("JN17 SetConnectedAnchor changes the field but never rebuilds",
     JOINT_CPP, "void JointComponent::SetConnectedAnchor(const Vector3& connected_anchor)",
     "    m_JointRes.m_ConnectedAnchor = connected_anchor;\n    RebuildJoint();",
     "    m_JointRes.m_ConnectedAnchor = connected_anchor;"),

    ("JN19 SetEnabled only flips the field, Jolt keeps solving",
     JOINT_CPP, "void JointComponent::SetEnabled(bool enabled)",
     "        physics_scene->SetJointEnabled(m_JointId, enabled);",
     "        (void)enabled;"),

    ("JN20 FixedTick breaks every joint on the first step",
     JOINT_CPP, "void JointComponent::FixedTick(float fixed_delta_time)",
     "    if (force <= m_JointRes.m_BreakForce && torque <= m_JointRes.m_BreakTorque)",
     "    if (false)"),

    # =========================================================================
    # HingeJointComponent
    # =========================================================================
    ("JN24 SetLimits stops clamping a positive min",
     HINGE_CPP, "void HingeJointComponent::SetLimits(const JointLimits& limits)",
     "    m_JointRes.m_MinLimit = limits.min > 0.f ? 0.f : limits.min;",
     "    m_JointRes.m_MinLimit = limits.min;"),

    ("JN25 SetLimits changes the field but never rebuilds",
     HINGE_CPP, "void HingeJointComponent::SetLimits(const JointLimits& limits)",
     "    if (m_JointRes.m_UseLimits)\n    {\n        RebuildJoint();\n    }",
     "    ;"),

    ("JN27 SetMotor always stores a zero force",
     HINGE_CPP, "void HingeJointComponent::SetMotor(const JointMotor& motor)",
     "    m_JointRes.m_MotorForce = motor.force < 0.f ? 0.f : motor.force;",
     "    m_JointRes.m_MotorForce = 0.f;"),

    # =========================================================================
    # SpringJointComponent / SliderJointComponent
    # =========================================================================
    ("JN28 SetSpring is a no-op",
     SPRING_CPP, "void SpringJointComponent::SetSpring(float spring)",
     "    m_JointRes.m_Spring = spring < 0.f ? 0.f : spring;",
     "    (void)spring;"),

    ("JN29 ApplyJointSettings pushes the un-set (-1) distances into Jolt",
     SPRING_CPP, "void SpringJointComponent::ApplyJointSettings()",
     "    if (m_JointRes.m_MinDistance >= 0.f && m_JointRes.m_MaxDistance >= 0.f)",
     "    if (true)"),

    ("JN30 SetMotorForce stops clamping a negative force",
     SLIDER_CPP, "void SliderJointComponent::SetMotorForce(float force)",
     "    m_JointRes.m_MotorForce = force < 0.f ? 0.f : force;",
     "    m_JointRes.m_MotorForce = force;"),

    # =========================================================================
    # 发布面 / 接线
    # =========================================================================
    ("JN31 the HingeJoint type alias disappears from the .d.ts",
     BINDING_CPP, None,
     '"declare type HingeJoint = HingeJointComponent;\\n"',
     '""'),

    ("JN32 the JointLimits interface is renamed away",
     BINDING_CPP, None,
     '"declare interface JointLimits {\\n"',
     '"declare interface JointLimitsRenamed {\\n"'),

    ("JN33 the marshaller for JointMotor is gone",
     BINDING_H, None,
     "    struct Converter<JointMotor>",
     "    struct Converter<JointMotorRenamed>"),

    ("JN34 the deferred joint flush is never called",
     WORLD_CPP, None,
     "    FlushPendingJoints(new_level);",
     "    ;"),

    ("JN35 the no-physics build loses CreateStaticAnchorBody",
     NULL_CPP, None,
     "uint32_t PhysicsScene::CreateStaticAnchorBody(const Vector3&)",
     "uint32_t PhysicsScene::CreateStaticAnchorBodyRenamed(const Vector3&)"),

    # =========================================================================
    # 退化限位（min == max，即"锁死"）
    # -------------------------------------------------------------------------
    # Jolt 在 Hinge/Slider 的构造函数里断言 `min != max || frequency > 0`。
    # 单元测试里 `AssertFailed` 是"打印 + 计入失败"，所以这四个变异体不是靠
    # 断言语句、而是靠**真的崩不出来**才被抓住的 —— 换句话说 JN41/JN42/JN43/
    # JN59b 是"这条 Jolt 断言不再触发"的回归门禁。
    # =========================================================================
    ("JN37 a locked hinge keeps frequency 0 (Jolt asserts 'use a fixed constraint')",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            // Jolt 断言限位区间跨 0，且两端不能相等（除非带弹簧，见\n"
     "            // `kDegenerateLimitFrequency` 的注释）。\n"
     "            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)\n"
     "            {\n"
     "                settings.mLimitsMin = in_desc.m_MinLimit;\n"
     "                settings.mLimitsMax = in_desc.m_MaxLimit;\n"
     "                if (settings.mLimitsMin == settings.mLimitsMax)\n"
     "                {\n"
     "                    settings.mLimitsSpringSettings.mFrequency = kDegenerateLimitFrequency;",
     "            // Jolt 断言限位区间跨 0，且两端不能相等（除非带弹簧，见\n"
     "            // `kDegenerateLimitFrequency` 的注释）。\n"
     "            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)\n"
     "            {\n"
     "                settings.mLimitsMin = in_desc.m_MinLimit;\n"
     "                settings.mLimitsMax = in_desc.m_MaxLimit;\n"
     "                if (settings.mLimitsMin == settings.mLimitsMax)\n"
     "                {\n"
     "                    settings.mLimitsSpringSettings.mFrequency = 0.f;"),

    ("JN38 a locked slider keeps frequency 0",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            // 0/0 同样是\"锁死\"，不该被静默丢弃成\"没有限位\"）。\n"
     "            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)\n"
     "            {\n"
     "                settings.mLimitsMin = in_desc.m_MinLimit;\n"
     "                settings.mLimitsMax = in_desc.m_MaxLimit;\n"
     "                if (settings.mLimitsMin == settings.mLimitsMax)\n"
     "                {\n"
     "                    settings.mLimitsSpringSettings.mFrequency = kDegenerateLimitFrequency;",
     "            // 0/0 同样是\"锁死\"，不该被静默丢弃成\"没有限位\"）。\n"
     "            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)\n"
     "            {\n"
     "                settings.mLimitsMin = in_desc.m_MinLimit;\n"
     "                settings.mLimitsMax = in_desc.m_MaxLimit;\n"
     "                if (settings.mLimitsMin == settings.mLimitsMax)\n"
     "                {\n"
     "                    settings.mLimitsSpringSettings.mFrequency = 0.f;"),

    ("JN39 the slider drops a 0/0 range again (silently 'no limits')",
     SCENE_CPP, "uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)",
     "            // 0/0 同样是\"锁死\"，不该被静默丢弃成\"没有限位\"）。\n"
     "            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)\n"
     "            {\n"
     "                settings.mLimitsMin = in_desc.m_MinLimit;\n"
     "                settings.mLimitsMax = in_desc.m_MaxLimit;\n"
     "                if (settings.mLimitsMin == settings.mLimitsMax)",
     "            // 0/0 同样是\"锁死\"，不该被静默丢弃成\"没有限位\"）。\n"
     "            if (in_desc.m_MinLimit < 0.f && in_desc.m_MaxLimit > 0.f)\n"
     "            {\n"
     "                settings.mLimitsMin = in_desc.m_MinLimit;\n"
     "                settings.mLimitsMax = in_desc.m_MaxLimit;\n"
     "                if (false)"),

    ("JN40 SetUseLimits flips the field but never rebuilds",
     HINGE_CPP, "void HingeJointComponent::SetUseLimits(bool use_limits)",
     "    m_JointRes.m_UseLimits = use_limits;\n"
     "    // 限位烘在约束里，只能重建（见 `FillJointLimits` 的注释）。\n"
     "    RebuildJoint();",
     "    m_JointRes.m_UseLimits = use_limits;"),
]

LOCK = os.path.join(BAK, "zengine_mutate_joints.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_joints.py is already running (%s)" % LOCK)
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
            # 只有动到 d.ts 模板（ScriptBinding.cpp 的 prelude）才需要整链；
            # 其余是代码，增量链接补得进去。
            global NEED_RELINK
            NEED_RELINK = path in (BINDING_CPP, BINDING_H)
            bak = os.path.join(BAK, os.path.basename(path) + ".jointbak")
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
                with io.open(os.path.join(ROOT, ".mutate_joints.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-70s %s\n" % (name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        # 收尾：源码都已复原，把 d.ts 也刷回真身（否则最后留下来的是变异体那份）。
        regen_dts()
        print("---- summary ----")
        for n, r in results:
            print("%-70s %s" % (n, r))
        # "编译不过" 同样算被杀掉（变异体根本进不了二进制），只是不进 caught 计数。
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
