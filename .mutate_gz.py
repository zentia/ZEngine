"""Mutation harness for the Gizmos slice.

Two things make this slice easy to fake:

  1. The backend counts primitives but says nothing about *which* primitive a
     call produced. `DrawCube` calling `DrawWireCube` would still "draw a
     cube". Hence the GZ13-GZ16 pair, which asserts each call lands in its own
     bucket and NOT in the other's.
  2. `AddQuad(..., _FillMode_solid)` compiles, counts zero and renders nothing.
     A mutant that goes back to quads must fail, and the gate that notices is
     the triangle count, not the compiler.
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

GIZMOS_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptGizmos\ScriptGizmos.h")
GIZMOS_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptGizmos\ScriptGizmos.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
VM_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptVMSmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def run(exe):
    r = subprocess.run([os.path.join(ROOT, exe)], cwd=ROOT, env=ENV,
                       capture_output=True, text=True, errors="replace", timeout=300)
    out = r.stdout or ""
    fails = [l for l in out.splitlines() if "FAIL" in l]
    tail = [l for l in out.splitlines() if ("checks," in l or "项检查" in l)]
    print("  %-28s rc=%d  %s" % (os.path.basename(exe), r.returncode,
                                 tail[-1].strip() if tail else "(crashed -- no summary)"))
    for l in fails[:4]:
        print("      " + l.strip())
    return r.returncode


def build():
    for _ in range(3):
        ok = True
        for target in ("UnityParitySmokeTest", "ScriptVMSmokeTest", "ScriptComponentSmokeTest"):
            p = subprocess.run(["cmake", "--build", ".", "--target", target,
                                "--config", "Debug"], cwd=os.path.join(ROOT, "build"),
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
        # 一个编译不过的变异体不是"漏测"——它根本没变成一个可运行的程序。
        # 记成 invalid 并重跑（本机 MSBuild 偶发 LNK1104 / MSB3371），连着三次
        # 都编译不过才是真的 invalid。
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(VM_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    ("GZ1 DrawRay treats `direction` as the endpoint", GIZMOS_CPP,
     "        const ::Vector3 end = from + direction;",
     "        const ::Vector3 end = direction;"),

    ("GZ2 DrawLine swaps its two endpoints", GIZMOS_CPP,
     "        group->AddLine(matrix * from, matrix * to, color, color,\n"
     "                       k_debug_draw_one_frame, kDepthTested);",
     "        group->AddLine(matrix * to, matrix * from, color, color,\n"
     "                       k_debug_draw_one_frame, kDepthTested);"),

    ("GZ3 the colour's r and b channels are swapped", GIZMOS_CPP,
     "        return ::Vector4(c.r, c.g, c.b, c.a);",
     "        return ::Vector4(c.b, c.g, c.r, c.a);"),

    ("GZ4 Gizmos.matrix is never applied", GIZMOS_CPP,
     "        group->AddLine(matrix * from, matrix * to, color, color,\n"
     "                       k_debug_draw_one_frame, kDepthTested);",
     "        group->AddLine(from, to, color, color,\n"
     "                       k_debug_draw_one_frame, kDepthTested);"),

    ("GZ5 SetColor writes a shadowing local (getter/setter diverge)", GIZMOS_CPP,
     "        s_Color = color;",
     "        ::Color s_Color = color;"),

    ("GZ6 DrawCube just calls DrawWireCube", GIZMOS_CPP,
     "        // ⚠️ 不能用 `AddQuad(..., _FillMode_solid)`",
     "        DrawWireCube(center, size); if (false) {}\n"
     "        // ⚠️ 不能用 `AddQuad(..., _FillMode_solid)`"),

    ("GZ7 DrawWireCube adds a sphere instead of a box", GIZMOS_CPP,
     "        group->AddBox(matrix * center, half, kNoRotation, ToVec4(GetColor()),\n"
     "                      k_debug_draw_one_frame, kDepthTested);",
     "        group->AddSphere(matrix * center, 1.0f, ToVec4(GetColor()),\n"
     "                         k_debug_draw_one_frame, kDepthTested);"),

    ("GZ8 gizmos are drawn always-on-top (not depth tested)", GIZMOS_CPP,
     "    constexpr bool kDepthTested = false;",
     "    constexpr bool kDepthTested = true;"),

    # ⚠️ 变异体必须能**编译**：早期版本只把 `AddTriangle(` 换成 `AddQuad(`，
    # 参数个数对不上直接编译失败 —— 那不是"被抓住"，是变异体无效。AddQuad 要
    # 4 个角 + 4 个颜色，所以整块替换（一个面一个 quad，而不是两个三角形）。
    ("GZ9 DrawCube goes back to solid quads (which render nothing)", GIZMOS_CPP,
     "            group->AddTriangle(corner[face[0]],\n"
     "                               corner[face[1]],\n"
     "                               corner[face[2]],\n"
     "                               color,\n"
     "                               color,\n"
     "                               color,\n"
     "                               k_debug_draw_one_frame,\n"
     "                               kDepthTested,\n"
     "                               _FillMode_solid);\n"
     "            group->AddTriangle(corner[face[0]],\n"
     "                               corner[face[2]],\n"
     "                               corner[face[3]],\n"
     "                               color,\n"
     "                               color,\n"
     "                               color,\n"
     "                               k_debug_draw_one_frame,\n"
     "                               kDepthTested,\n"
     "                               _FillMode_solid);",
     "            group->AddQuad(corner[face[0]],\n"
     "                           corner[face[1]],\n"
     "                           corner[face[2]],\n"
     "                           corner[face[3]],\n"
     "                           color,\n"
     "                           color,\n"
     "                           color,\n"
     "                           color,\n"
     "                           k_debug_draw_one_frame,\n"
     "                           kDepthTested,\n"
     "                           _FillMode_solid);"),

    ("GZ10 DrawLine draws nothing at all", GIZMOS_CPP,
     "        group->AddLine(matrix * from, matrix * to, color, color,\n"
     "                       k_debug_draw_one_frame, kDepthTested);",
     "        (void)matrix; (void)from; (void)to; (void)color;"),

    ("GZ11 the facade never reaches the backend", GIZMOS_CPP,
     "        return manager->TryGetOrCreateDebugDrawGroup(kGizmoGroupName);",
     "        return nullptr;"),

    ("GZ12 the group name is misspelled (tests look up \"gizmos\")", GIZMOS_CPP,
     "    constexpr const char* kGizmoGroupName = \"gizmos\";",
     "    constexpr const char* kGizmoGroupName = \"gizmo\";"),

    ("GZ13 DrawWireSphere forwards to DrawWireCube", GIZMOS_CPP,
     "        group->AddSphere(matrix * center, radius, ToVec4(GetColor()),\n"
     "                         k_debug_draw_one_frame, kDepthTested);",
     "        DrawWireCube(center, ::Vector3(radius, radius, radius));"),

    ("GZ14 SetMatrix never stores the matrix", GIZMOS_CPP,
     "        s_Matrix = matrix;",
     "        ::Matrix4x4 s_Matrix = matrix;"),

    ("GZ15 DrawWireCube passes the full size where half is meant", GIZMOS_CPP,
     "        const ::Vector3 half = size * 0.5f;\n\n        group->AddBox(",
     "        const ::Vector3 half = size * 1.0f;\n\n        group->AddBox("),
]

# GZ15 is expected to survive: the box primitive stores half-extents but the
# only observable count (12 wire edges) does not depend on them, and no public
# accessor exposes the extents. Recorded rather than smoothed over -- closing
# it needs a geometry accessor on DebugDrawGroup.
EQUIVALENT = {"GZ15"}


LOCK = os.path.join(BAK, "zengine_mutate_gz.lock")


def main():
    # 两个 harness 同时跑会互相踩 MSBuild（MSB3371 access denied + LNK1104
    # cannot open ZRuntime.lib），而且都往同一个 log 写 —— 结果会混在一起，看
    # 上去像"一半的变异体没被抓住"。用一个锁文件挡住第二个实例。
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_gz.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = sys.argv[1] if len(sys.argv) > 1 else None
    results = []
    touched = set()
    try:
        for name, path, old, new in MUTATIONS:
            if only and name.split()[0] != only:
                continue
            print("=== %s ===" % name)
            bak = os.path.join(BAK, os.path.basename(path) + ".gzbak")
            subprocess.run(["git", "show", "HEAD:" + os.path.relpath(path, ROOT).replace("\\", "/")],
                           cwd=ROOT, env=ENV, stdout=io.open(bak, "wb"), stderr=subprocess.DEVNULL)
            try:
                mutate(path, old, new)
                # 本机 MSBuild 偶发 LNK1104 / MSB3371，重试最多 3 次；三次都编译
                # 不过才判 invalid（变异体本身不合法）。
                verdict = "BUILD FAILED"
                for _ in range(3):
                    verdict = build_and_run()
                    if verdict != "BUILD FAILED":
                        break
                    print("  (retrying -- flaky MSBuild)")
                if verdict == "BUILD FAILED":
                    verdict = "invalid (does not build)"
                results.append((name, verdict))
            finally:
                shutil.copy2(bak, path)
                os.utime(path, None)
                touched.add(path)
            print()
        for path in touched:
            os.utime(path, None)
        print("---- summary ----")
        for n, r in results:
            tag = n.split()[0]
            verdict = ("equivalent (expected)" if tag in EQUIVALENT else "NOT CAUGHT") if r == "NOT CAUGHT" else r
            print("%-64s %s" % (n, verdict))
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
