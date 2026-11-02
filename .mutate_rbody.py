"""Mutation harness for the Rigidbody completion slice (#55).

What is under test
------------------
`RigidBodyComponent` grew Unity's `constraints` / `freezeRotation`, the centre
of mass and the inertia tensor, `collisionDetectionMode`, `maxAngularVelocity`,
the two point-velocity queries, `AddRelativeForce` / `AddRelativeTorque` and
`SetDensity`. `PhysicsScene` grew the body-state accessors behind them, and
`RigidBody.h` grew the two Unity enums plus the bit mapping.

Why these particular mutants
----------------------------
* **The bit mapping is the one thing here that is easy to get subtly wrong.**
  Unity's `RigidbodyConstraints` says "which axes are FROZEN" and starts
  translation at bit 1; Jolt's `EAllowedDOFs` says "which axes are ALLOWED" and
  starts translation at bit 0. RB01/RB02/RB05 move a single bit; they are caught
  only because the test pins *each* axis separately (RB2/RB3/RB4) on a fixture
  where no two axes are interchangeable.
* **Two different "position" quantities.** Jolt's `Body::GetPosition()` is the
  body ORIGIN (`mPosition - rotation * localCOM`) while
  `GetCenterOfMassPosition()` is `mPosition`. RB11 replaces one with the other;
  it is caught only because the fixture has a body whose shape COM is 0.25 off
  centre -- on a symmetric box the two are identical and the mutant is
  equivalent.
* **`SetMass` must carry the current DOF mask through.** RB08 is the exact bug
  this slice fixed: `SetMassProperties` takes the mask and the mass together, so
  writing `EAllowedDOFs::All` there silently clears the constraints.
* **Point velocity is the whole point of `GetPointVelocity`.** RB16 collapses it
  to the linear velocity, which is what you get if you forget the cross product.
* **Several component-level mutants have no behavioural witness** (the component
  fixture has no live body), so they are pinned by source-level wiring gates
  (RB76/RB77/RB78) instead. That is a documented weakness, not an oversight:
  without a WorldManager-published active scene there is no way to give the
  component a body from this test binary.

Deliberately NOT mutated (equivalent mutants -- they would survive and that is
a fact about the test, not a bug in the code):
  * `GetCollisionDetectionMode` ignoring the live body: with no body the
    fallback is the stored value, so it is indistinguishable.
  * `SetBodyAllowedDOFs` ignoring the `mass` argument: nothing downstream reads
    the mass after it in these tests.

Run:  py .mutate_rbody.py            (all)
      py .mutate_rbody.py RB01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


RES_H = S(r"engine/Source/Runtime/Resource/ResType/Components/RigidBody.h")
SCENE_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsScene.cpp")
COMP_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.cpp")
NULL_CPP = S(r"engine/Source/Runtime/Function/Physics/PhysicsSceneNull.cpp")
BINDING_CPP = S(r"engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp")

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


TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


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
    # =========================================================================
    # 位映射（RigidBody.h 里的纯函数）
    # =========================================================================
    ("RB01 the X translation bit maps onto Jolt's Y bit",
     RES_H, None,
     "        dofs |= 1u << 0;  // TranslationX",
     "        dofs |= 1u << 1;  // TranslationX"),

    ("RB02 the Z rotation bit maps onto Jolt's X rotation bit",
     RES_H, None,
     "        dofs |= 1u << 5;  // RotationZ",
     "        dofs |= 1u << 3;  // RotationZ"),

    ("RB05 ToJoltAllowedDOFs ignores its argument (always all six)",
     RES_H, None,
     "    uint32_t dofs = 0;",
     "    uint32_t dofs = 0x3f;"),

    ("RB03 ToRigidbodyConstraints always reports None",
     RES_H, None,
     "    return static_cast<RigidbodyConstraints>(constraints);",
     "    return RigidbodyConstraints::None;"),

    # 反向映射里的"缺位 = 冻住"规则。直接喂一个缺位掩码才分得出 —— 写进去再读
    # 回来的掩码本来就完整。
    ("RB06 ToRigidbodyConstraints treats a missing DOF bit as free",
     RES_H, None,
     "    if ((in_jolt_dofs & (1u << 3)) == 0)",
     "    if (false)"),

    ("RB04 UsesContinuousCollision always says discrete",
     RES_H, None,
     "    return in_mode != CollisionDetectionMode::Discrete;",
     "    return false;"),

    # =========================================================================
    # PhysicsScene：约束
    # =========================================================================
    ("RB07 SetBodyAllowedDOFs ignores the mask and always allows all six",
     SCENE_CPP, "bool PhysicsScene::SetBodyAllowedDOFs(uint32_t body_id, uint32_t dof_mask, float mass)",
     "static_cast<JPH::EAllowedDOFs>(dof_mask & 0x3fu)",
     "JPH::EAllowedDOFs::All"),

    # 本片修掉的那个真 bug：改质量把约束清了。
    ("RB08 SetMass resets the DOF mask to All",
     SCENE_CPP, "bool PhysicsScene::SetMass(uint32_t body_id, float mass)",
     "motion_properties->SetMassProperties(motion_properties->GetAllowedDOFs(), mass_properties);",
     "motion_properties->SetMassProperties(JPH::EAllowedDOFs::All, mass_properties);"),

    # =========================================================================
    # PhysicsScene：质心 / 惯性 / 体积
    # =========================================================================
    ("RB10 GetBodyCenterOfMass always reports zero",
     SCENE_CPP, "bool PhysicsScene::GetBodyCenterOfMass(uint32_t body_id, Vector3& out_local_center_of_mass) const",
     "out_local_center_of_mass = toVec3(shape->GetCenterOfMass());",
     "out_local_center_of_mass = Vector3::ZERO;"),

    ("RB11 GetBodyWorldCenterOfMass reports the body ORIGIN instead of the COM",
     SCENE_CPP, "bool PhysicsScene::GetBodyWorldCenterOfMass(uint32_t body_id, Vector3& out_world_center_of_mass) const",
     "toVec3(body_lock.GetBody().GetCenterOfMassPosition())",
     "toVec3(body_lock.GetBody().GetPosition())"),

    ("RB09 GetBodyInertiaTensor reports the INVERSE inertia",
     SCENE_CPP, "bool PhysicsScene::GetBodyInertiaTensor(uint32_t body_id, Vector3& out_inertia, Quaternion& out_rotation) const",
     "    out_inertia = Vector3(inverse_diagonal.GetX() > 0.f ? 1.f / inverse_diagonal.GetX() : 0.f,\n"
     "                          inverse_diagonal.GetY() > 0.f ? 1.f / inverse_diagonal.GetY() : 0.f,\n"
     "                          inverse_diagonal.GetZ() > 0.f ? 1.f / inverse_diagonal.GetZ() : 0.f);",
     "    out_inertia = Vector3(inverse_diagonal.GetX(), inverse_diagonal.GetY(), inverse_diagonal.GetZ());"),

    ("RB26 GetBodyInertiaTensorRotation returns a non-unit quaternion",
     SCENE_CPP, "bool PhysicsScene::GetBodyInertiaTensor(uint32_t body_id, Vector3& out_inertia, Quaternion& out_rotation) const",
     "out_rotation = toQuat(motion_properties->GetInertiaRotation());",
     "out_rotation = toQuat(motion_properties->GetInertiaRotation()) * 2.0f;"),

    ("RB12 GetBodyVolume always reports 1 m3",
     SCENE_CPP, "bool PhysicsScene::GetBodyVolume(uint32_t body_id, float& out_volume) const",
     "out_volume = shape->GetVolume();",
     "out_volume = 1.0f;"),

    # =========================================================================
    # CCD / 角速度上限 / 点速度
    # =========================================================================
    ("RB13 SetBodyMotionQuality never turns continuous on",
     SCENE_CPP, "bool PhysicsScene::SetBodyMotionQuality(uint32_t body_id, CollisionDetectionMode mode)",
     "UsesContinuousCollision(mode) ? JPH::EMotionQuality::LinearCast",
     "false ? JPH::EMotionQuality::LinearCast"),

    ("RB14 GetBodyMotionQuality always reports Discrete",
     SCENE_CPP, "bool PhysicsScene::GetBodyMotionQuality(uint32_t body_id, CollisionDetectionMode& out_mode) const",
     "    out_mode = (motion_properties->GetMotionQuality() == JPH::EMotionQuality::LinearCast)\n"
     "                   ? CollisionDetectionMode::Continuous\n"
     "                   : CollisionDetectionMode::Discrete;",
     "    out_mode = CollisionDetectionMode::Discrete;"),

    ("RB27 GetBodyMaxAngularVelocity reports a constant",
     SCENE_CPP, "bool PhysicsScene::GetBodyMaxAngularVelocity(uint32_t body_id, float& out_max_angular_velocity) const",
     "out_max_angular_velocity = motion_properties->GetMaxAngularVelocity();",
     "out_max_angular_velocity = 10.0f;"),

    ("RB29 SetBodyMaxAngularVelocity clamps to a fixed value",
     SCENE_CPP, "bool PhysicsScene::SetBodyMaxAngularVelocity(uint32_t body_id, float max_angular_velocity)",
     "const float safe_max = (max_angular_velocity > kMinMass) ? max_angular_velocity : kMinMass;",
     "const float safe_max = 10.0f;"),

    ("RB15 SetBodyMaxAngularVelocity is a no-op",
     SCENE_CPP, "bool PhysicsScene::SetBodyMaxAngularVelocity(uint32_t body_id, float max_angular_velocity)",
     "motion_properties->SetMaxAngularVelocity(safe_max);",
     "(void)safe_max;"),

    ("RB16 GetBodyPointVelocity reports the linear velocity (drops the spin)",
     SCENE_CPP, "bool PhysicsScene::GetBodyPointVelocity(uint32_t body_id, const Vector3& world_point, Vector3& out_velocity) const",
     "out_velocity = toVec3(jph_body.GetPointVelocity(JPH::RVec3(toVec3(world_point))));",
     "out_velocity = toVec3(jph_body.GetLinearVelocity());"),

    # =========================================================================
    # 组件层
    # =========================================================================
    ("RB17 SetConstraints forgets to remember what was set",
     COMP_CPP, "void RigidBodyComponent::SetConstraints(RigidbodyConstraints constraints)",
     "    m_Constraints = constraints;",
     "    (void)constraints;"),

    ("RB18 SetFreezeRotation wipes the position freeze bits",
     COMP_CPP, "void RigidBodyComponent::SetFreezeRotation(bool freeze_rotation)",
     "constraints &= ~static_cast<int>(RigidbodyConstraints::FreezeRotation);",
     "constraints = 0;"),

    ("RB19 SetFreezeRotation only freezes the X axis",
     COMP_CPP, "void RigidBodyComponent::SetFreezeRotation(bool freeze_rotation)",
     "constraints |= static_cast<int>(RigidbodyConstraints::FreezeRotation);",
     "constraints |= static_cast<int>(RigidbodyConstraints::FreezeRotationX);"),

    ("RB28 SetCollisionDetectionMode forgets to remember what was set",
     COMP_CPP, "void RigidBodyComponent::SetCollisionDetectionMode(CollisionDetectionMode mode)",
     "    m_CollisionDetectionMode = mode;",
     "    (void)mode;"),

    ("RB20 AddRelativeForce drops the rotation (becomes AddForce)",
     COMP_CPP, "void RigidBodyComponent::AddRelativeForce(const Vector3& force)",
     "AddForce(GetRotation() * force);",
     "AddForce(force);"),

    ("RB21 SetDensity ignores the volume",
     COMP_CPP, "void RigidBodyComponent::SetDensity(float density)",
     "SetMass(density * volume);",
     "SetMass(density);"),

    ("RB22 GetRelativePointVelocity measures from the COM instead of the origin",
     COMP_CPP, "Vector3 RigidBodyComponent::GetRelativePointVelocity(const Vector3& local_point) const",
     "GetPosition() + GetRotation() * local_point",
     "GetWorldCenterOfMass() + GetRotation() * local_point"),

    # body 重建之后要把约束补回去：少一条路径就是"关卡一加载约束就没了"。
    ("RB24 the pending flush does not re-apply the constraint",
     COMP_CPP, "bool RigidBodyComponent::FlushPendingBody()",
     "    ApplyRuntimeBodySettings();\n    return true;",
     "    return true;"),

    ("RB25 SetConstraints passes Unity's bits through as Jolt's mask",
     COMP_CPP, "void RigidBodyComponent::SetConstraints(RigidbodyConstraints constraints)",
     "ToJoltAllowedDOFs(constraints)",
     "static_cast<uint32_t>(constraints)"),

    # =========================================================================
    # 发布面 / 无物理构建
    # =========================================================================
    ("RB23b the Rigidbody type alias is gone",
     BINDING_CPP, None,
     '"declare type Rigidbody = RigidBodyComponent;\\n"',
     '""'),

    ("RB23 the no-physics build loses GetBodyPointVelocity",
     NULL_CPP, None,
     "bool PhysicsScene::GetBodyPointVelocity(uint32_t, const Vector3&, Vector3& out_velocity) const",
     "bool PhysicsScene::GetBodyPointVelocityRenamed(uint32_t, const Vector3&, Vector3& out_velocity) const"),
]

LOCK = os.path.join(BAK, "zengine_mutate_rbody.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_rbody.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".rbodybak")
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
                with io.open(os.path.join(ROOT, ".mutate_rbody.log"), "a", encoding="utf-8") as lg:
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
