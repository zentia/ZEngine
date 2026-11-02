"""Mutation harness for the CharacterController slice (AA/AB checks)."""
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
CTRL_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Controller\CharacterController.cpp")
CTRL_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Controller\CharacterController.h")
MOTOR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Motor\MotorComponent.cpp")
MOTOR_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Motor\MotorComponent.h")
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
    ("M1 height is the cylinder, not the capsule", CTRL_CPP,
     "    return 2.0f * (m_Capsule->m_HalfHeight + m_Capsule->m_Radius);",
     "    return 2.0f * m_Capsule->m_HalfHeight;"),
    ("M2 SetHeight forgets to keep the radius", CTRL_CPP,
     "    m_Capsule->m_HalfHeight = 0.5f * clamped - m_Capsule->m_Radius;",
     "    m_Capsule->m_HalfHeight = 0.5f * clamped;"),
    ("M3 a zero radius is accepted", CTRL_CPP,
     "    m_Capsule->m_Radius = (radius > kMinCapsuleRadius) ? radius : kMinCapsuleRadius;",
     "    m_Capsule->m_Radius = radius;"),
    ("M4 dimension setters forget the shape transform", CTRL_CPP,
     "    m_RigidbodyShape->m_LocalTransform = TransformTRS(\n"
     "        Vector3(0, 0, m_Capsule->m_HalfHeight + m_Capsule->m_Radius), orientation, Vector3::UNIT_SCALE);",
     "    (void)orientation;"),
    ("M5 skinWidth is stored but not used", CTRL_CPP,
     "    return scene->Raycast(origin, Vector3::NEGATIVE_UNIT_Z, half_extent + skin, hits);",
     "    return scene->Raycast(origin, Vector3::NEGATIVE_UNIT_Z, half_extent, hits);"),
    ("M6 grounding reads the jump state machine instead of probing", CTRL_CPP,
     "    return scene->Raycast(origin, Vector3::NEGATIVE_UNIT_Z, half_extent + skin, hits);",
     "    return true;"),
    ("M7 Move is a single combined sweep (no per-axis, no sliding)", CTRL_CPP,
     "    if (std::fabs(motion.z) > kEpsilon)\n    {\n        const Vector3 stepped =\n"
     "            detect_collisions ? MoveInScene(scene, position, Vector3(0.f, 0.f, motion.z))\n"
     "                              : position + Vector3(0.f, 0.f, motion.z);",
     "    if (false)\n    {\n        const Vector3 stepped = position;"),
    ("M8 a blocked downward move reports nothing", CTRL_CPP,
     "        flags |= (motion.z < 0.f) ? kFlagBelow : kFlagAbove;",
     "        flags |= 0;"),
    ("M9 minMoveDistance is ignored", CTRL_CPP,
     "    if (motion.length() <= min_move_distance)\n    {\n        return 0;\n    }",
     "    if (false)\n    {\n        return 0;\n    }"),
    ("M10 detectCollisions=false still collides", CTRL_CPP,
     "        const Vector3 stepped =\n            detect_collisions ? MoveInScene(scene, position, Vector3(0.f, 0.f, motion.z))\n"
     "                              : position + Vector3(0.f, 0.f, motion.z);",
     "        const Vector3 stepped = MoveInScene(scene, position, Vector3(0.f, 0.f, motion.z));"),
    ("M11 SimpleMove honours the vertical speed", CTRL_CPP,
     "    const Vector3 horizontal = Vector3(speed.x, speed.y, 0.f);",
     "    const Vector3 horizontal = speed;"),
    ("M12 the fall speed never resets on landing", CTRL_CPP,
     "        inout_fall_speed = 0.f;\n        return horizontal * delta_time;",
     "        return horizontal * delta_time;"),
    ("M13 SimpleMove does not multiply by delta time", CTRL_CPP,
     "        return horizontal * delta_time;",
     "        return horizontal;"),
    ("M14 a method is silently not exported", MOTOR_H,
     "    ZFUNCTION()\n    void SimpleMove(const Vector3& speed);",
     "    void SimpleMove(const Vector3& speed);"),
    ("M15 Move re-implements the sweep instead of delegating", MOTOR_CPP,
     "    const int32_t flags = controller->MoveWithFlags(ResolvePhysicsScene(), start, motion,\n"
     "                                                    m_DetectCollisions, m_MinMoveDistance, landed);",
     "    landed = start + motion;\n    const int32_t flags = 0;"),
    ("M16 CollisionFlags uses the wrong bit values", MOTOR_H,
     "    sides = 1,\n    above = 2,\n    below = 4",
     "    sides = 0,\n    above = 1,\n    below = 2"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".ccbak")
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
