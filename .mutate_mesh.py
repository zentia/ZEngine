"""Mutation harness for the Mesh / MeshFilter slice (#53).

What is under test
------------------
`Mesh` is a CPU-side mesh data object (Unity's `new Mesh()` shape): vertices /
triangles / normals / tangents / UVs plus `RecalculateBounds`,
`RecalculateNormals`, `RecalculateTangents`. `MeshFilter` is the component that
feeds a sibling `BaseRenderer` -- the asset path goes into the sub-mesh list
(that is the part that actually draws), a runtime `Mesh` only into
`SetLocalBounds` (that is the part culling reads).

Why these particular mutants
----------------------------
* **Mesh is where "the data API really computes" has to be pinned.** Every
  mutant here is a *wrong number*, not a crash: the tangent sign, the
  handedness bit, the winding, the bounds corners. A suite that only asks
  "did it not crash" catches none of them.
* **Two mutants exist specifically because the obvious fixture cannot tell the
  implementations apart.** MF11 (w is always +1) needs a flipped-v UV layout to
  be observable -- with the obvious layout the right answer *is* +1. MF15 (only
  the first vertex of each triangle accumulates) needs all four corners
  checked, not just [0].
* **MF08 is about which guard is actually load-bearing.** Removing the "skip
  degenerate faces" test changes nothing on its own (a zero-length face normal
  adds zero); what stops NaN is `SafeNormalise` refusing to divide by zero. So
  the mutant attacks the guard that does the work.
* **MeshFilter is a wiring component, so half the mutants are "the write-back
  went missing".** MF16/MF17/MF19/MF20 each delete one half of the write-back;
  a component that only stores its own field is indistinguishable from an
  unimplemented one, and it fails silently.
* **MF21/MF22 are the add-order path.** `AddComponent` runs PostLoadResource
  before the component is in the object's list, so "filter first, renderer
  second" needs the pending flag and the retry in Tick. Removing either one
  drops the mesh on the floor with no error anywhere.

Run:  py .mutate_mesh.py            (all)
      py .mutate_mesh.py MF01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"


def S(p):
    return os.path.join(ROOT, p.replace("/", os.sep))


MESH_H = S(r"engine/Source/Runtime/Scripting/ScriptMesh/ScriptMesh.h")
MESH_CPP = S(r"engine/Source/Runtime/Scripting/ScriptMesh/ScriptMesh.cpp")
FILTER_CPP = S(r"engine/Source/Runtime/Function/Framework/Component/Mesh/MeshFilter.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# .cpp 里的目标函数在文件作用域，函数收尾是顶格的 `}`。
END_OF_FUNC = "\n}\n"
# ⚠️ 类里的内联函数：函数体 8 空格，收尾是 4 空格的 `}`。用顶格标记会一路跑到
# 类外面，把无关代码也改掉。
END_OF_CLASS_FUNC = "\n    }\n"


def marker_for(path):
    return END_OF_CLASS_FUNC if path.endswith((".h",)) else END_OF_FUNC


TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
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
                # LNK1236 是 ZRuntime.lib 的 COFF 段被写坏了（MSBuild 并行撞车），
                # 不是代码错。单编一次 ZRuntime 再继续。
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
    # Mesh：规模与索引
    # =========================================================================
    ("MF01 GetTriangleCount reports the index count instead of triangles",
     MESH_H, None,
     "int GetTriangleCount() const { return static_cast<int>(m_Triangles.size() / 3); }",
     "int GetTriangleCount() const { return static_cast<int>(m_Triangles.size()); }"),

    # ⚠️ 少了这条，"长度不是 3 的倍数"会被原样存下：GetTriangleCount 说 length/3，
    # 画的却是一个残缺三角形 —— 而残缺三角形在渲染侧是读越界。
    ("MF02 SetTriangles keeps a trailing partial triangle",
     MESH_H, "void SetTriangles(const std::vector<int32_t>& triangles)",
     "        if (m_Triangles.size() % 3 != 0)\n        {\n"
     "            m_Triangles.resize((m_Triangles.size() / 3) * 3);\n        }",
     "        ;"),

    ("MF03 an empty Mesh claims one sub-mesh",
     MESH_H, None,
     "int GetSubMeshCount() const { return m_Triangles.empty() ? 0 : 1; }",
     "int GetSubMeshCount() const { return 1; }"),

    # =========================================================================
    # Mesh：包围盒
    # =========================================================================
    # 换顶点之后不标脏 => bounds 留着上一批顶点的结果。这是"看似对"的那类错。
    ("MF04 SetVertices leaves the cached bounds valid",
     MESH_H, "void SetVertices(const std::vector<Vector3>& vertices)",
     "        m_BoundsDirty = true;", "        m_BoundsDirty = false;"),

    # 空网格退回"单位立方体"：那是一个完全合理的包围盒，于是"没算出来"和
    # "算出来了"分不开 —— 宁可明显地错（零盒），不可看似对地错。
    ("MF05 an empty Mesh's bounds is the unit cube instead of the zero box",
     MESH_CPP, "Bounds Mesh::ComputeBounds() const",
     "        return Bounds();",
     "        return Bounds(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));"),

    ("MF06 ComputeBounds only tracks the min corner",
     MESH_CPP, "Bounds Mesh::ComputeBounds() const",
     "        min_corner.makeFloor(vertex);\n        max_corner.makeCeil(vertex);",
     "        min_corner.makeFloor(vertex);"),

    # =========================================================================
    # Mesh：法线
    # =========================================================================
    ("MF07 the accumulated face normal is negated",
     MESH_CPP, "void Mesh::RecalculateNormals()",
     "        m_Normals[i0] += face_normal;", "        m_Normals[i0] -= face_normal;"),

    # ⚠️ 变异的是 SafeNormalise 的零长度守卫，不是"跳过退化面"那个判据：
    # 后者去掉之后结果一模一样（零长度的面法线加进来等于没加），真正挡住 NaN
    # 的是这里。变异一个没有可观测差别的位置只会得到一个等价变异体。
    ("MF08 a zero-length vector normalises into NaN",
     MESH_CPP, "Vector3 SafeNormalise(const Vector3& v)",
     "        if (squared <= 0.0f)\n        {\n            return Vector3::ZERO;\n        }",
     "        if (false)\n        {\n            return Vector3::ZERO;\n        }"),

    # =========================================================================
    # Mesh：切线
    # =========================================================================
    ("MF09 RecalculateTangents runs without normals or UVs (reads out of range)",
     MESH_CPP, "void Mesh::RecalculateTangents()",
     "    if (m_Normals.size() != m_Vertices.size() || m_UVs.size() != m_Vertices.size())",
     "    if (false)"),

    ("MF10 the tangent formula is flipped",
     MESH_CPP, "void Mesh::RecalculateTangents()",
     "        const Vector3 tangent = (edge0 * dv1 - edge1 * dv0) * reciprocal;",
     "        const Vector3 tangent = (edge1 * dv0 - edge0 * dv1) * reciprocal;"),

    # ⚠️ 只有 ME46（翻转 v 的那一条）抓得住它：默认布局下手性本来就是 +1。
    ("MF11 tangent.w is always +1 (the handedness bit is never computed)",
     MESH_CPP, "void Mesh::RecalculateTangents()",
     "handedness < 0.0f ? -1.0f : 1.0f", "1.0f"),

    ("MF12 GetUVs ignores the channel and always returns channel 0",
     MESH_H, None,
     "std::vector<Vector2> GetUVs(int channel) const { return channel == 0 ? m_UVs : std::vector<Vector2>(); }",
     "std::vector<Vector2> GetUVs(int channel) const { return m_UVs; }"),

    ("MF13 SetUVs claims to accept every channel",
     MESH_H, "bool SetUVs(int channel, const std::vector<Vector2>& uvs)",
     "        if (channel != 0)\n        {\n            return false;\n        }",
     "        if (false)\n        {\n            return false;\n        }"),

    # =========================================================================
    # Mesh：Clear 与累加
    # =========================================================================
    ("MF14 Clear forgets the index buffer",
     MESH_H, "void Clear()",
     "        m_Triangles.clear();", "        ;"),

    ("MF15 only the first vertex of each triangle accumulates a tangent",
     MESH_CPP, "void Mesh::RecalculateTangents()",
     "        tangents[i0] += tangent;\n        tangents[i1] += tangent;\n        tangents[i2] += tangent;",
     "        tangents[i0] += tangent;"),

    # =========================================================================
    # MeshFilter：写回渲染器（接线）
    # =========================================================================
    ("MF16 the sub-mesh write-back is gone",
     FILTER_CPP, "void MeshFilter::PushToRenderer()",
     "    renderer->SetSubMeshes(sub_meshes);", "    ;"),

    ("MF17 SetSharedMeshPath only stores the field",
     FILTER_CPP, "void MeshFilter::SetSharedMeshPath(const eastl::string& mesh_asset)",
     "    m_MeshAsset = mesh_asset;\n    PushToRenderer();", "    m_MeshAsset = mesh_asset;"),

    ("MF19 SetMesh does not push the bounds",
     FILTER_CPP, "void MeshFilter::SetMesh(Mesh* mesh)",
     "    m_RuntimeMesh = mesh;\n    PushToRenderer();", "    m_RuntimeMesh = mesh;"),

    ("MF20 clearing the runtime mesh leaves the local-bounds override behind",
     FILTER_CPP, "void MeshFilter::PushToRenderer()",
     "        renderer->ClearLocalBoundsOverride();", "        ;"),

    ("MF21 a failed write-back is not remembered",
     FILTER_CPP, "void MeshFilter::PushToRenderer()",
     "        m_PushPending = true;\n        return;", "        return;"),

    ("MF22 Tick never retries the pending push",
     FILTER_CPP, "void MeshFilter::Tick(float delta_time)",
     "    if (m_PushPending)\n    {\n        PushToRenderer();\n    }",
     "    if (false)\n    {\n        PushToRenderer();\n    }"),

    ("MF23 GetLocalBounds always reports the zero box",
     FILTER_CPP, "Bounds MeshFilter::GetLocalBounds() const",
     "    return (runtime_mesh != nullptr) ? runtime_mesh->GetBounds() : Bounds();",
     "    return Bounds();"),

    ("MF25 HasRenderer always says yes",
     FILTER_CPP, "bool MeshFilter::HasRenderer() const",
     "    return FindRenderer() != nullptr;", "    return true;"),

    ("MF26 GetMesh always reports null",
     FILTER_CPP, "Mesh* MeshFilter::GetMesh()",
     "    return m_RuntimeMesh;", "    return nullptr;"),
]

LOCK = os.path.join(BAK, "zengine_mutate_mesh.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_mesh.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".mshbak")
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
                with io.open(os.path.join(ROOT, ".mutate_mesh.log"), "a", encoding="utf-8") as lg:
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
