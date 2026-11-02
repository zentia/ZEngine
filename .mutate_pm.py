"""Mutation harness for the PhysicMaterial slice (AO / AP / AQ checks).

Same contract as the other .mutate_*.py: every mutation must make the smoke
test go red. Anything that survives is an assertion that is not actually
watching the thing it claims to watch.

⚠️ Restore ends with `touch`. MSBuild's timestamp check has been observed to
leave a stale .obj behind after a fast mutate -> build -> restore -> build
round trip, which then crashes at a point that has nothing to do with the
source. Touching is what keeps the last build honest.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
F_RES = os.path.join(ROOT, r"engine\Source\Runtime\Resource\ResType\Components\RigidBody.h")
F_SCENE = os.path.join(ROOT, r"engine\Source\Runtime\Function\Physics\PhysicsScene.cpp")
F_COMP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Framework\Component\Rigidbody\RigidbodyComponent.cpp")
EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    text = io.open(path, encoding="utf-8").read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def build_and_run():
    p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0 and ("LNK1236" in p.stdout or "LNK1000" in p.stdout):
        subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
        p = subprocess.run(["cmake", "--build", ".", "--target", "UnityParitySmokeTest", "--config", "Debug"],
                           cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
    if p.returncode != 0:
        print("  BUILD FAILED rc=%d" % p.returncode)
        for line in p.stdout.splitlines()[-6:]:
            print("    " + line)
        return "build-failed"
    r = subprocess.run([os.path.join(ROOT, EXE)], cwd=ROOT, env=ENV, capture_output=True, text=True, timeout=300)
    fails = [l for l in r.stdout.splitlines() if "FAIL" in l]
    tail = [l for l in r.stdout.splitlines() if "checks," in l]
    print("  rc=%d  %s" % (r.returncode, tail[-1] if tail else "(crashed -- no summary)"))
    for l in fails[:8]:
        print("    " + l.strip())
    if r.returncode == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("M1 Average adds instead of averaging", F_RES,
     "            return 0.5f * (value_a + value_b);",
     "            return value_a + value_b;"),

    ("M2 Minimum takes the larger", F_RES,
     "        case PhysicMaterialCombine::Minimum:\n"
     "            return (value_a < value_b) ? value_a : value_b;",
     "        case PhysicMaterialCombine::Minimum:\n"
     "            return (value_a > value_b) ? value_a : value_b;"),

    ("M3 Multiply adds", F_RES,
     "        case PhysicMaterialCombine::Multiply:\n"
     "            return value_a * value_b;",
     "        case PhysicMaterialCombine::Multiply:\n"
     "            return value_a + value_b;"),

    ("M4 priority inverts (lowest wins, not highest)", F_RES,
     "    const PhysicMaterialCombine effective = (combine_a > combine_b) ? combine_a : combine_b;",
     "    const PhysicMaterialCombine effective = (combine_a < combine_b) ? combine_a : combine_b;"),

    ("M5 an out-of-range combine mode reads as Maximum", F_RES,
     "            return PhysicMaterialCombine::Average;",
     "            return PhysicMaterialCombine::Maximum;"),

    ("M6 the two combine modes share the same user-data bits", F_RES,
     "           (static_cast<uint64_t>(ToSerializedCombineMode(bounce_combine)) << 8);",
     "           (static_cast<uint64_t>(ToSerializedCombineMode(bounce_combine)) << 0);"),

    ("M7 the default friction is not Unity's 0.6", F_RES,
     "    float m_Friction {0.6f};",
     "    float m_Friction {0.2f};"),

    ("M8 CreateRigidBody ignores the authored friction", F_SCENE,
     "    body_settings.mFriction = rigidbody_actor_res.m_Friction;",
     "    body_settings.mFriction = 0.2f;"),

    ("M9 the combine callbacks are never installed on Jolt", F_SCENE,
     "    m_Physics.m_JoltPhysicsSystem->SetCombineFriction(&JoltCombineFriction);",
     "    // mutated: combine callback not installed"),

    ("M10 SetBodyFriction reports success without writing", F_SCENE,
     "    body_interface.SetFriction(JPH::BodyID(body_id), friction);\n"
     "    return true;",
     "    return true;"),

    ("M11 SetFriction stops clamping negatives", F_COMP,
     "    m_RigidbodyRes.m_Friction = (friction > 0.f) ? friction : 0.f;",
     "    m_RigidbodyRes.m_Friction = friction;"),

    ("M12 SetBounciness stops clamping to 0..1", F_COMP,
     "    m_RigidbodyRes.m_Bounciness = (bounciness > 0.f) ? ((bounciness < 1.f) ? bounciness : 1.f) : 0.f;",
     "    m_RigidbodyRes.m_Bounciness = bounciness;"),

    ("M13 setting the friction combine resets the bounce combine", F_COMP,
     "    m_RigidbodyRes.SetFrictionCombine(combine_mode);",
     "    m_RigidbodyRes.SetFrictionCombine(combine_mode);\n"
     "    m_RigidbodyRes.SetBounceCombine(PhysicMaterialCombine::Average);"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".pmbak")
        shutil.copy2(path, bak)
        try:
            mutate(path, old, new)
            results.append((name, build_and_run()))
        finally:
            shutil.copy2(bak, path)
            os.utime(path, None)  # see module docstring: never leave a stale .obj
            touched.add(path)
        print()
    for path in touched:
        os.utime(path, None)
    print("---- summary ----")
    for n, r in results:
        print("%-58s %s" % (n, r))


if __name__ == "__main__":
    main()
