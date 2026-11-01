"""Mutation harness for `Gizmos.DrawFrustum`.

The interesting mutants here are the AXIS ones (FRM01/FRM02/FRM03). Copying
Unity's literal `+Z` opening direction yields a shape that is still 12 lines,
still a truncated pyramid, still symmetric about the centre -- every counting
assertion stays green. Only a signed assertion ("the far plane sits at -Y")
separates them, which is what FR7 is for and what this harness proves.

Run:  py .mutate_fr.py            (all)
      py .mutate_fr.py FRM01       (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

GZ_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptGizmos\ScriptGizmos.h")
GZ_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptGizmos\ScriptGizmos.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
VM_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptVMSmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n    }\n"

# ⚠️ 一次只编一个 target。
# 同时给 `--target A B` 会让 MSBuild 并行跑，而某个 target 的 post-build 会往
# bin/Debug/ZRuntime.lib 拷一份 —— 那个拷贝正好撞上另一个 target 的链接，
# 于是间歇性 LNK1236（"corrupt or invalid COFF sections"）。一个个编就没这问题。
TARGETS = ("UnityParitySmokeTest", "ScriptVMSmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new, count=1):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    if count == -1:
        text = text.replace(old, new)
    else:
        for _ in range(count):
            text = text.replace(old, new, 1)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)


def mutate_in(path, func_sig, old, new):
    """Replace only inside one function body.

    锚点必须限定在函数内：`const ::Vector4 color = ToVec4(GetColor());` 在
    ScriptGizmos.cpp 里有四份（DrawLine / DrawRay / DrawCube / DrawFrustum），
    全局 replace 会改到别的入口上，得到一个"什么也没验证"的 SURVIVED。
    """
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    j = text.find(END_OF_FUNC, i)
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
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(VM_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # ---- 轴向（这一组存在的理由）------------------------------------------
    ("FRM01 the frustum opens along +Y instead of -Y", GZ_CPP,
     "        const ::Vector3& forward = ::Vector3::NEGATIVE_UNIT_Y;",
     "        const ::Vector3& forward = ::Vector3::UNIT_Y;"),

    ("FRM02 the frustum opens along Unity's literal +Z", GZ_CPP,
     "        const ::Vector3& forward = ::Vector3::NEGATIVE_UNIT_Y;",
     "        const ::Vector3& forward = ::Vector3::UNIT_Z;"),

    ("FRM03 up is +Y instead of +Z", GZ_CPP,
     "        const ::Vector3& up = ::Vector3::UNIT_Z;",
     "        const ::Vector3& up = ::Vector3::UNIT_Y;"),

    # ---- 12 条边：三个循环各一次 --------------------------------------------
    ("FRM04 the near-plane loop is gone (8 lines)", GZ_CPP,
     "            add_edge(near_c[i], near_c[j]);  // 近平面", ""),

    ("FRM05 the far-plane loop is gone (8 lines)", GZ_CPP,
     "            add_edge(far_c[i], far_c[j]);  // 远平面", ""),

    ("FRM06 the side-edge loop is gone (8 lines)", GZ_CPP,
     "            add_edge(near_c[i], far_c[i]);  // 侧棱", ""),

    # ---- 参数 ---------------------------------------------------------------
    ("FRM07 aspect is ignored (width == height)", GZ_CPP,
     "* tan_half_fov * aspect)", "* tan_half_fov)", -1),

    ("FRM08 fov is treated as radians", GZ_CPP,
     "std::tan(Math::DegreesToRadians(fov) * 0.5f)", "std::tan(fov * 0.5f)"),

    ("FRM09 the degenerate-parameter guard is gone (inf/NaN)", GZ_CPP,
     "        if (!(fov > 0.0f) || fov >= 180.0f || !(near_clip_plane > 0.0f) ||\n"
     "            !(far_clip_plane > near_clip_plane) || !(aspect > 0.0f))\n"
     "        {\n            return;\n        }\n",
     ""),

    ("FRM10 the half-angle is the full angle", GZ_CPP,
     "std::tan(Math::DegreesToRadians(fov) * 0.5f)", "std::tan(Math::DegreesToRadians(fov))"),

    # ---- Gizmos.matrix / Gizmos.color / 深度测试 -----------------------------
    ("FRM11 Gizmos.matrix is not applied", GZ_CPP,
     "            near_c[i] = matrix * (center", "            near_c[i] = (center"),

    ("FRM12 Gizmos.color is ignored", GZ_CPP,
     "void Gizmos::DrawFrustum(",
     "        const ::Vector4 color = ToVec4(GetColor());",
     "        const ::Vector4 color = ::Vector4(1.0f, 1.0f, 1.0f, 1.0f);"),

    ("FRM13 the frustum is drawn always-on-top", GZ_CPP,
     "void Gizmos::DrawFrustum(",
     "k_debug_draw_one_frame, kDepthTested);",
     "k_debug_draw_one_frame, true);"),

    # ---- 接线门（GW11 本身）-------------------------------------------------
    # 只改空白：声明还在、还能编译，但 GW11 数 `static void DrawFrustum(` 会是 0。
    ("FRM14 the declaration is spelled differently", GZ_H,
     "        static void DrawFrustum(", "        static void  DrawFrustum("),
]

LOCK = os.path.join(BAK, "zengine_mutate_fr.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_fr.py is already running (%s)" % LOCK)
    io.open(LOCK, "w", encoding="utf-8").write("%d\n" % os.getpid())

    only = set(sys.argv[1:])
    results = []
    touched = set()
    try:
        for entry in MUTATIONS:
            # (name, path, old, new[, count]) 或 (name, path, func_sig, old, new)
            name, path, a, b = entry[0], entry[1], entry[2], entry[3]
            count = 1
            func_sig = None
            if len(entry) == 5:
                if entry[4] == -1 or isinstance(entry[4], int):
                    count = entry[4]
                else:
                    func_sig = a
                    a, b = entry[3], entry[4]
            if only and name.split()[0] not in only:
                continue
            print("=== %s ===" % name)
            # 备份**工作副本**（同 .mutate_vt.py 的理由：提交后 == HEAD，且不受
            # .gitignore 影响）。
            bak = os.path.join(BAK, os.path.basename(path) + ".frbak")
            shutil.copy2(path, bak)
            try:
                if func_sig is not None:
                    mutate_in(path, func_sig, a, b)
                else:
                    mutate(path, a, b, count)
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
            print("%-64s %s" % (n, r))
        bad = [n for n, r in results if r != "caught"]
        print("\n%d/%d caught" % (len(results) - len(bad), len(results)))
        for n in bad:
            print("  SURVIVED: " + n)
    finally:
        os.remove(LOCK)


if __name__ == "__main__":
    main()
