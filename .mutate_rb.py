"""Mutation harness for the Rigidbody slice (G/O checks)."""
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
SCENE_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Physics\PhysicsScene.cpp")
COMP_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Rigidbody\RigidbodyComponent.cpp")
COMP_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Rigidbody\RigidbodyComponent.h")
EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%s" % (path, old))
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new, 1))


def build_and_run():
    p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest",
                        "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
                       env=ENV, capture_output=True, text=True)
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines()[-6:]:
            print("    " + line)
        return "build-failed"
    r = subprocess.run([os.path.join(ROOT, EXE)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, timeout=300)
    fails = [l for l in r.stdout.splitlines() if "FAIL" in l or "ASSERT" in l]
    tail = [l for l in r.stdout.splitlines() if "checks," in l]
    print("  rc=%d  %s" % (r.returncode, tail[-1] if tail else ""))
    for l in fails[:10]:
        print("    " + l.strip())
    if r.returncode == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("M1 mass ignores the inertia tensor", SCENE_CPP,
     "    motion_properties->SetMassProperties(JPH::EAllowedDOFs::All, mass_properties);",
     "    motion_properties->SetInverseMass(1.f / safe_mass);"),
    ("M2 a zero mass is accepted", SCENE_CPP,
     "    const float safe_mass = (mass > kMinMass) ? mass : kMinMass;",
     "    const float safe_mass = mass;"),
    ("M3 a zero delta_time reaches MoveKinematic", SCENE_CPP,
     "    const float safe_delta = (delta_time > kMinDeltaTime) ? delta_time : kMinDeltaTime;",
     "    const float safe_delta = delta_time;"),
    ("M4 AddForceAtPosition ignores the position", SCENE_CPP,
     "    body_interface.AddForce(JPH::BodyID(body_id), toVec3(force), toVec3(position));",
     "    body_interface.AddForce(JPH::BodyID(body_id), toVec3(force));"),
    ("M5 AddTorque is a no-op", SCENE_CPP,
     "    body_interface.AddTorque(JPH::BodyID(body_id), toVec3(torque));",
     "    (void)torque;"),
    ("M6 useGravity always reads true", COMP_CPP,
     "    return physics_scene->GetGravityFactor(m_RigidbodyId, factor) ? (factor > 0.f) : true;",
     "    return true;"),
    ("M7 setDrag is a no-op", COMP_CPP,
     "        physics_scene->SetLinearDamping(m_RigidbodyId, drag);",
     "        (void)drag;"),
    ("M8 SleepRigidBody is a no-op", SCENE_CPP,
     "    body_interface.DeactivateBody(JPH::BodyID(body_id));",
     "    (void)body_id;"),
    ("M9 SetMass forgets the serialized field", COMP_CPP,
     "    m_RigidbodyRes.m_InverseMass = (mass > 0.f) ? (1.f / mass) : 0.f;",
     ""),
    ("M10 a method is silently not exported", COMP_H,
     "    ZFUNCTION()\n    void AddForceAtPosition(const Vector3& force, const Vector3& position);",
     "    void AddForceAtPosition(const Vector3& force, const Vector3& position);"),
    ("M11 activate re-enters the body write lock", SCENE_CPP,
     "    JPH::BodyLockRead probe_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(),\n                                     JPH::BodyID(body_id));",
     "    JPH::BodyLockWrite probe_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(),\n                                      JPH::BodyID(body_id));"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".rbbak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
        print()
    print("---- summary ----")
    for name, res in results:
        print("%-52s %s" % (name, res))


if __name__ == "__main__":
    main()
