"""Mutation harness for `Plane` / `GeometryUtility` (the PL / PLW / Y slice).

Why this slice needs its own harness:

* `Plane.h` and `Frustum.h` have existed for a long time and are correct. What
  did not exist was the *script* half -- `Converter<Plane>`, the `declare
  interface`, and the two facades. So most of what this harness guards is the
  facade layer's own arithmetic, and every trap there is a plausible wrong
  number:
  - `distance`'s sign (PLM01 / PLM11). Flip it and every frustum's inside/outside
    inverts while `TestPlanesAABB` still returns a perfectly plausible bool.
  - `Flip` negating only the normal (PLM03): the result is still a self-consistent
    unit-normal plane, it is just a *different* plane.
  - `Translate`'s `-dot` vs `+dot` (PLM04): the plane moves the wrong way and
    nothing complains.
  - `Raycast` returning 0 instead of -1 for "miss" (PLM02) collides with "hit
    exactly at the origin".
* `TestPlanesAABB` is the one function where "check only the first plane" and
  "pick the wrong corner" both survive a small fixture, so PL39/PL40 (every axis
  in turn) and PL35 (a straddling box) are load-bearing.
* The wiring gates (PLW1/PLW3/PLW6) are the only thing that can see the
  publishing half at all.

Known coverage gap (deliberately NOT written as a mutant):

* `Mover.ts` is not recompiled by this harness, so the TS half (Y1) has no
  mutant of its own. Every number Y1 asserts, however, comes from a C++ function
  that does: `-3` from PLM01, `4` from PLM02's sibling, `-1` from PLM02, and the
  trailing `true|false` from PLM05/PLM06.

Run:  py .mutate_pl.py            (all)
      py .mutate_pl.py PLM01      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

PLANE_H = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\Plane.h")
FACADE_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptPlane\ScriptPlane.cpp")
FACADE_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptPlane\ScriptPlane.h")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

END_OF_FUNC = "\n    }\n"

# ⚠️ 一次只编一个 target：同时给 `--target A B` 会让 MSBuild 并行跑，某个 target
# 的 post-build 往 bin/Debug/ZRuntime.lib 拷贝时正好撞上另一个的链接 —— 间歇性
# LNK1236。
TARGETS = ("UnityParitySmokeTest", "ScriptComponentSmokeTest")


def mutate(path, old, new):
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if old not in text:
        raise SystemExit("PATTERN NOT FOUND in %s:\n%r" % (path, old))
    io.open(path, "w", encoding="utf-8", newline="\n").write(text.replace(old, new, 1))


def mutate_in(path, func_sig, old, new, end_marker=None):
    """Replace only inside one function body (see .mutate_tc.py for why)."""
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()
    i = text.find(func_sig)
    if i < 0:
        raise SystemExit("FUNC NOT FOUND in %s:\n%r" % (path, func_sig))
    marker = end_marker if end_marker is not None else END_OF_FUNC
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
        return "BUILD FAILED"
    if run(PARITY_EXE) | run(COMP_EXE) == 0:
        print("  >>> NOT CAUGHT <<<")
        return "NOT CAUGHT"
    return "caught"


MUTATIONS = [
    # ---- ⚠️ 头条：distance 的符号 -------------------------------------------
    # FromNormalAndPosition 不归一化法线 ⇒ distance 也不对，"距离"被法线长度缩放。
    # 只有给一条**非单位**法线才看得出来（PL4 / PL5）。
    ("PLM01 FromNormalAndPosition forgets to normalise (distance is scaled too)",
     FACADE_CPP, "::Plane Plane::FromNormalAndPosition",
     "        return ::Plane(normal, point);",
     "        return ::Plane(normal, -normal.dotProduct(point));"),

    # 反过来：`Create` 自作主张归一化（Unity 的构造函数**不**归一化）。
    ("PLM02b Create normalises the normal (Unity's ctor deliberately does not)",
     FACADE_CPP, "::Plane Plane::Create",
     "        return ::Plane(normal, distance);",
     "        ::Plane out(normal, distance);\n        out.Normalize();\n        return out;"),

    # ---- Raycast ------------------------------------------------------------
    # "没打中"返回 0 ⇒ 与"正好打在起点上"（t = 0）撞车。
    ("PLM02 Raycast reports a miss as 0 instead of -1", FACADE_CPP, "float Plane::Raycast",
     "            return -1.f;", "            return 0.f;"),

    # ---- 非破坏的"改" --------------------------------------------------------
    # 只翻 normal、不翻 distance：结果仍是自洽的平面，只是换了一个。
    ("PLM03 Flip negates only the normal (it moves the plane)", FACADE_CPP,
     "::Plane Plane::Flipped",
     "        ::Plane out = plane;\n        out.Flip();",
     "        ::Plane out = plane;\n        out.normal = -out.normal;"),

    # `distance -= dot(normal, t)` 写成 `+=`：平面往反方向跑。
    ("PLM04 Translate adds the dot product instead of subtracting it", PLANE_H,
     "        distance -= normal.dotProduct(translation);",
     "        distance += normal.dotProduct(translation);"),

    # 零法线上做除法 ⇒ NaN。"返回零平面"的守卫没了。
    ("PLM12 Normalize no longer guards against a zero normal", PLANE_H,
     "        if (len < 1e-6f)\n        {\n            return;\n        }\n", ""),

    # 三点绕序反过来 ⇒ 法线朝另一边。
    ("PLM16 Set3Points crosses in the wrong order", PLANE_H,
     "SetNormalAndPosition((b - a).crossProduct(c - a), a);",
     "SetNormalAndPosition((c - a).crossProduct(b - a), a);"),

    # ---- 值类型本身 ----------------------------------------------------------
    # 边界上的点不算正面（Unity 与 `SameSide` 都是 `>=`）。
    ("PLM09 GetSide excludes the boundary (it becomes > 0)", PLANE_H,
     "        return GetDistanceToPoint(point) >= 0.f;",
     "        return GetDistanceToPoint(point) > 0.f;"),

    # SameSide 与 GetSide 用不同的边界规则：边界点被判成"两侧都不在"。
    ("PLM10 SameSide uses a different boundary rule than GetSide", PLANE_H,
     "        return (d0 >= 0.f && d1 >= 0.f) || (d0 < 0.f && d1 < 0.f);",
     "        return (d0 > 0.f && d1 > 0.f) || (d0 < 0.f && d1 < 0.f);"),

    # 最近点往法线方向推而不是拉回来。
    ("PLM13 ClosestPointOnPlane pushes along the normal instead of back", PLANE_H,
     "        return point - normal * GetDistanceToPoint(point);",
     "        return point + normal * GetDistanceToPoint(point);"),

    # ---- GeometryUtility -----------------------------------------------------
    # ⚠️ 只判第一个平面：只测"整体在外"的夹具照样全绿，靠 PL39 / PL40（逐个轴）。
    ("PLM05 TestPlanesAABB only looks at the first plane", FACADE_CPP,
     "bool GeometryUtility::TestPlanesAABB",
     "            if (plane.GetDistanceToPoint(positive_vertex) < 0.f)\n"
     "            {\n"
     "                return false;\n"
     "            }",
     "            return plane.GetDistanceToPoint(positive_vertex) >= 0.f;"),

    # 取"负顶点"而不是"正顶点"：跨过平面的盒子（PL35）会翻成 false。
    ("PLM06 TestPlanesAABB picks the corner nearest the back instead of the front",
     FACADE_CPP, "bool GeometryUtility::TestPlanesAABB",
     "            const Vector3 positive_vertex(n.x >= 0.f ? max_corner.x : min_corner.x,\n"
     "                                          n.y >= 0.f ? max_corner.y : min_corner.y,\n"
     "                                          n.z >= 0.f ? max_corner.z : min_corner.z);",
     "            const Vector3 positive_vertex(n.x >= 0.f ? min_corner.x : max_corner.x,\n"
     "                                          n.y >= 0.f ? min_corner.y : max_corner.y,\n"
     "                                          n.z >= 0.f ? min_corner.z : max_corner.z);"),

    # 拿不到相机时返回 6 个"默认"平面：后面的 TestPlanesAABB 会静默地恒真或恒假。
    ("PLM07 CalculateFrustumPlanes returns six planes for a null camera", FACADE_CPP,
     "std::vector<::Plane> GeometryUtility::CalculateFrustumPlanes(Camera* camera)",
     "            return std::vector<::Plane>();", "            return std::vector<::Plane>(6);"),

    # 深度区间用成 OpenGL 的 -1..1：near 平面整个错位（PL42 逐条比对六个面）。
    ("PLM08 CalculateFrustumPlanesVP assumes the OpenGL depth range", FACADE_CPP,
     "Frustum::DepthRange::kZeroToOne", "Frustum::DepthRange::kNegOneToOne"),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ 只有源码门 PLW6 看得见：多一个空格不影响任何运行期行为，但说明那条
    # "Unity 形状"的签名随时可能被改掉而无人察觉。
    ("PLM14 CalculateFrustumPlanes' signature drifts from Unity's", FACADE_H,
     "static std::vector<::Plane> CalculateFrustumPlanes(Camera* camera);",
     "static std::vector<::Plane> CalculateFrustumPlanes (Camera* camera);"),

    # ⚠️ 只有源码门 PLW3 看得见：少了 .d.ts 声明不影响 C++，但用户工程里
    # `Plane` 这个名字根本不存在。
    ("PLM15 the Plane .d.ts declaration is gone", BINDING_CPP,
     '        "declare interface Plane {\\n"\n', ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_pl.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_pl.py is already running (%s)" % LOCK)
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
            # 备份**工作副本**（不是 HEAD）：这次的改动还没提交。
            bak = os.path.join(BAK, os.path.basename(path) + ".plbak")
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
                with io.open(os.path.join(ROOT, ".mutate_pl.log"), "a", encoding="utf-8") as lg:
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
