"""Mutation harness for the Collider slice (AC/AD checks)."""
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"
COL_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptCollider\ScriptCollider.cpp")
COL_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptCollider\ScriptCollider.h")
PHYS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Function\Physics\Physics.cpp")
QUERY_H = os.path.join(ROOT, r"engine\Source\Runtime\Function\Physics\PhysicsQuery.h")
BIND_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.h")
BIND_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")
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
    if p.returncode != 0 and "LNK1236" in p.stdout:
        # MSVC 的偶发 "corrupt or invalid COFF sections"：重编一次 ZRuntime 就好。
        print("  (LNK1236 -- rebuilding ZRuntime and retrying)")
        subprocess.run(["cmake", "--build", ".", "--target", "ZRuntime", "--config", "Debug"],
                       cwd=os.path.join(ROOT, "build"), env=ENV, capture_output=True, text=True)
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
    ("M1 FromGameObject returns null when there is no RigidBody", COL_CPP,
     "        auto created = std::make_unique<Collider>();\n        created->SetOwner(owner);",
     "        if (owner->GetComponentByTypeName(\"RigidBodyComponent\") == nullptr) { return nullptr; }\n"
     "        auto created = std::make_unique<Collider>();\n        created->SetOwner(owner);"),
    ("M2 the cache is dropped (a fresh view per call)", COL_CPP,
     "        auto created = std::make_unique<Collider>();\n        created->SetOwner(owner);\n"
     "        Collider* raw = created.get();\n        cache.emplace(owner, std::move(created));\n        return raw;",
     "        auto created = std::make_unique<Collider>();\n        created->SetOwner(owner);\n"
     "        return created.release();"),
    ("M3 BoxCollider.size reports half extents", COL_CPP,
     "        return box->m_HalfExtents * 2.0f;",
     "        return box->m_HalfExtents;"),
    ("M4 capsule height forgets the caps", COL_CPP,
     "        return 2.0f * (capsule->m_HalfHeight + capsule->m_Radius);",
     "        return 2.0f * capsule->m_HalfHeight;"),
    ("M5 bounds ignore the shape's local offset", COL_CPP,
     "        const Vector3 world_center = object_position + object_rotation * local_center;",
     "        const Vector3 world_center = object_position;"),
    ("M6 bounds ignore the object's rotation", COL_CPP,
     "        const Quaternion world_rotation = object_rotation * local_rotation;",
     "        const Quaternion world_rotation = local_rotation;"),
    ("M7 bounds use only the first shape", COL_CPP,
     "        bounds.Merge(world_center - reach);\n    }\n\n    return bounds;",
     "        bounds.Merge(world_center - reach);\n        break;\n    }\n\n    return bounds;"),
    ("M8 out-of-range index is not guarded", COL_CPP,
     "    if (index < 0 || static_cast<size_t>(index) >= shapes.size())\n    {\n"
     "        return static_cast<int32_t>(ColliderShapeType::invalid);\n    }",
     "    (void)index;"),
    ("M9 ClosestPoint returns zero instead of the point when invalid", COL_CPP,
     "        return point;\n    }\n    const Vector3 min_corner",
     "        return Vector3::ZERO;\n    }\n    const Vector3 min_corner"),
    ("M10 [equivalent] isTrigger guesses true with no scene", COL_CPP,
     "    std::shared_ptr<PhysicsScene> scene = CurrentScene();\n    if (!scene)\n    {\n        return false;\n    }",
     "    std::shared_ptr<PhysicsScene> scene = CurrentScene();\n    if (!scene)\n    {\n        return true;\n    }"),
    ("M11 IsValid is hard-coded true", COL_CPP,
     "    return rigidbody != nullptr && !rigidbody->GetRigidbodyRes().m_Shapes.empty();",
     "    return true;"),
    ("M12 the funnel forgets to fill gameObject", PHYS_CPP,
     "        out.gameObject = ResolveBodyOwner(in_hit.body_id);",
     "        (void)0;"),
    ("M13 the converter drops gameObject", BIND_H,
     '            apis->set_property(env, o, "gameObject", Converter<GameObject*>::ToJs(apis, env, v.gameObject));',
     "            (void)0;"),
    ("M14 the .d.ts forgets gameObject", BIND_CPP,
     '        "    gameObject: GameObject | null;\\n"',
     "        "),
    ("M15 FromBodyId drops the owner lookup", COL_CPP,
     "    return ColliderFor(manager->FindBodyOwner(body_id));",
     "    return ColliderFor(nullptr);"),
    ("M16 a method is silently not exported", COL_H,
     "    ZFUNCTION()\n    Vector3 ClosestPoint(const Vector3& point) const;",
     "    Vector3 ClosestPoint(const Vector3& point) const;"),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    for name, path, old, new in MUTATIONS:
        if only and only not in name.split()[0]:
            continue
        print("=== %s ===" % name)
        bak = os.path.join(BAK, os.path.basename(path) + ".colbak")
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
