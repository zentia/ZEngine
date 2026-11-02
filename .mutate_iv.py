"""Mutation harness for `Vector2Int` / `Vector3Int` / `Rect` / `RectInt` (IV / RC / X slice).

Why this slice needs its own harness:

* Every headline trap here is a *plausible wrong number*, and each one hides
  under the obvious fixture:
  - `FloorToInt` vs truncation differ ONLY on negatives (IVM01). A fixture made
    of 1.7 / -1.2 style positives is green either way.
  - `RoundToInt`'s tie rule (banker's) only shows on exact `.5` (IVM02).
  - `magnitude` returning int is invisible on 3-4-5 (IVM03) and on any integral
    Pythagorean triple, which is exactly what a "nice" test fixture is.
  - `Rect.xMin` returning `x` is invisible on a positive-width rect (RCM01) --
    and positive-width is the only shape a casual fixture uses.
  - `Rect.Contains` closed vs half-open differs only AT the max corner (RCM02).
  - `Overlaps` strict vs non-strict differs only when two rects share an edge
    (RCM03) -- and "overlapping" fixtures always genuinely overlap.
* The publishing half is invisible to arithmetic: the four `declare interface`
  blocks are the only reason these names type-check in a user project at all
  (RCM14), and `VectorInt.h`'s `float GetMagnitude()` is only pinned by the
  source gate RCW8.

Known coverage gap (deliberately NOT written as a mutant):

* `Mover.ts` is not recompiled by this harness, so a mutant there would be
  invisible to both exes. The TS half (X1/X3) is instead covered indirectly:
  every number it asserts comes from a C++ function that has a mutant here
  (IVM01 -> X1's "-1", RCM02/RCM03 -> X3's "false|...|false").

Run:  py .mutate_iv.py            (all)
      py .mutate_iv.py IVM01      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

VECTORINT_H = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\VectorInt.h")
RECT_H = os.path.join(ROOT, r"engine\Source\Runtime\Core\Math\Rect.h")
INTVEC_CPP = os.path.join(
    ROOT, r"engine\Source\Runtime\Scripting\ScriptIntVector\ScriptIntVector.cpp")
RECT_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptRect\ScriptRect.cpp")
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
    # ---- ⚠️ 头条：floor 不是截断 --------------------------------------------
    # 只在负数上分得开（IV16 的 -0.5 → -1），正数上两者完全相同。
    ("IVM01 FloorToInt truncates instead of flooring", INTVEC_CPP,
     "return ::Vector2Int(static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)));",
     "return ::Vector2Int(static_cast<int>(v.x), static_cast<int>(v.y));"),

    # 平局取偶 → 平局远离零。只有 `.5` 分得开（IV18 / IV18b）。
    ("IVM02 RoundToInt breaks ties away from zero instead of to even", INTVEC_CPP,
     "return ::Vector2Int(static_cast<int>(std::rint(v.x)), static_cast<int>(std::rint(v.y)));",
     "return ::Vector2Int(static_cast<int>(std::round(v.x)), static_cast<int>(std::round(v.y)));"),

    # magnitude 返回 int：3-4-5 上正好是 5，看不出来（IV1b 用 (1,1) → √2）。
    ("IVM03 Vector2Int.magnitude is truncated to an int", VECTORINT_H,
     "float GetMagnitude() const { return std::sqrt(static_cast<float>(x * x + y * y)); }",
     "float GetMagnitude() const "
     "{ return static_cast<float>(static_cast<int>(std::sqrt(static_cast<float>(x * x + y * y)))); }"),

    # Distance 是 float 同理（IV33 用 (1,1) → √2）。
    ("IVM05 Vector2Int.Distance is truncated to an int", INTVEC_CPP,
     "return std::sqrt(static_cast<float>(dx * dx + dy * dy));",
     "return static_cast<float>(static_cast<int>(std::sqrt(static_cast<float>(dx * dx + dy * dy))));"),

    # Min 在 Y 上取成了 Max：两个输入混着用的夹具（IV30）才看得见。
    ("IVM04 Vector2Int.Min takes the max on Y", INTVEC_CPP,
     "return ::Vector2Int((a.x < b.x) ? a.x : b.x, (a.y < b.y) ? a.y : b.y);",
     "return ::Vector2Int((a.x < b.x) ? a.x : b.x, (a.y > b.y) ? a.y : b.y);"),

    # ToVector2 把两个分量换错了位置。
    ("IVM06 Vector2Int.ToVector2 swaps its components", INTVEC_CPP,
     "return Vector2(static_cast<float>(v.x), static_cast<float>(v.y));",
     "return Vector2(static_cast<float>(v.y), static_cast<float>(v.x));"),

    # ⚠️ 常量用的是 Unity 的 Y-up 轴向。写成 ZEngine 的 up 就与 forward 撞了
    # （IV25 / IV25b）。
    ("IVM07 Vector3Int.forward is ZEngine's up instead of Unity's", INTVEC_CPP,
     "::Vector3Int Vector3Int::GetForward() { return ::Vector3Int(0, 0, 1); }",
     "::Vector3Int Vector3Int::GetForward() { return ::Vector3Int(0, 1, 0); }"),

    # 分量乘退化成标量乘。
    ("IVM08 Vector2Int.Scale multiplies by b.x only", INTVEC_CPP,
     "::Vector2Int Vector2Int::Scale(const ::Vector2Int& a, const ::Vector2Int& b) { return a * b; }",
     "::Vector2Int Vector2Int::Scale(const ::Vector2Int& a, const ::Vector2Int& b) { return a * b.x; }"),

    # Clamp 只夹上界：低位分量就漏出去了（IV32 的 y = -9 → 应变成 0）。
    ("IVM09 Vector2Int.Clamp drops the upper bound", INTVEC_CPP,
     "return Min(Max(v, min), max);", "return Min(v, max);"),

    # ⚠️ 这一条只有**源码门** RCW8 看得见：Vector3Int 没有"非整勾股数"的
    # magnitude 夹具，所以行为测试拦不住它。
    ("IVM10 Vector3Int.magnitude is declared int", VECTORINT_H,
     "float GetMagnitude() const { return std::sqrt(static_cast<float>(x * x + y * y + z * z)); }",
     "int GetMagnitude() const { return static_cast<int>(std::sqrt(static_cast<float>(x * x + y * y + z * z))); }"),

    # 方向常量的符号写反。
    ("IVM11 Vector2Int.left points right", INTVEC_CPP,
     "::Vector2Int Vector2Int::GetLeft() { return ::Vector2Int(-1, 0); }",
     "::Vector2Int Vector2Int::GetLeft() { return ::Vector2Int(1, 0); }"),

    # ---- ⚠️ 头条：负 width 的矩形 --------------------------------------------
    # `xMin` 直接返回 `x`：正 width 的矩形上完全一致（RC1 用 width = -10）。
    ("RCM01 Rect.xMin returns x instead of the lower edge", RECT_H,
     "T GetXMin() const { return (x + width < x) ? (x + width) : x; }",
     "T GetXMin() const { return x; }"),

    # ⚠️ 闭区间：只差在**正好落在 max 边上**的那个点（RC9 / RC40 / X3）。
    ("RCM02 Rect.Contains is closed at the high end (it becomes Bounds.Contains)",
     RECT_H,
     "return point_x >= GetXMin() && point_x < GetXMax() &&\n"
     "               point_y >= GetYMin() && point_y < GetYMax();",
     "return point_x >= GetXMin() && point_x <= GetXMax() &&\n"
     "               point_y >= GetYMin() && point_y <= GetYMax();"),

    # 非严格 Overlaps：只差在"两个矩形共享一条边"（RC15 / RC43 / X3）。
    ("RCM03 Rect.Overlaps is non-strict (edge sharing counts as overlap)", RECT_H,
     "return other.GetXMax() > GetXMin() && other.GetXMin() < GetXMax() &&\n"
     "               other.GetYMax() > GetYMin() && other.GetYMin() < GetYMax();",
     "return other.GetXMax() >= GetXMin() && other.GetXMin() <= GetXMax() &&\n"
     "               other.GetYMax() >= GetYMin() && other.GetYMin() <= GetYMax();"),

    # 半开区间的另一半：低端也必须是 >=（RC8 的 (0,0)）。
    ("RCM05 Rect.Contains excludes its own low corner", RECT_H,
     "return point_x >= GetXMin() && point_x < GetXMax() &&",
     "return point_x > GetXMin() && point_x < GetXMax() &&"),

    # ClampToBounds 用裸 width 而不是两条边的跨度：负尺寸的矩形就漏出去了（RC49）。
    ("RCM04 ClampToBounds uses the raw width instead of the normalised span", RECT_H,
     "T clamped_width = GetXMax() - GetXMin();", "T clamped_width = width;"),

    # OrderMinMax 变成恒等：翻转负尺寸的矩形这一步没了（RC28）。
    ("RCM06 Rect.OrderMinMax is a no-op", RECT_CPP, "::Rect Rect::OrderMinMax",
     "        ::Rect out;\n"
     "        out.x = r.GetXMin();\n"
     "        out.y = r.GetYMin();\n"
     "        out.width = r.GetXMax() - r.GetXMin();\n"
     "        out.height = r.GetYMax() - r.GetYMin();\n"
     "        return out;",
     "        return r;"),

    # 归一化坐标镜像了：中点仍是 0.5，只有四分之一点分得开（RC30b）。
    ("RCM07 PointToNormalized measures from the wrong edge", RECT_CPP,
     "Vector2 Rect::PointToNormalized",
     "((p.x - r.GetXMin()) / span_x)", "((r.GetXMax() - p.x) / span_x)"),

    # center 取成了 max 角。
    ("RCM08 Rect.center returns the max corner", RECT_CPP, "Vector2 Rect::GetCenter",
     "return Vector2(r.x + r.width * 0.5f, r.y + r.height * 0.5f);",
     "return Vector2(r.x + r.width, r.y + r.height);"),

    # ⚠️ 整数除法：奇数尺寸的 RectInt 中心会落在整格上（RC38 / X3）。
    ("RCM11 RectInt.center uses integer division", RECT_CPP, "Vector2 RectInt::GetCenter",
     "return Vector2(r.x + r.width * 0.5f, r.y + r.height * 0.5f);",
     "return Vector2(static_cast<float>(r.x + r.width / 2), "
     "static_cast<float>(r.y + r.height / 2));"),

    # ---- 非破坏 setter --------------------------------------------------------
    # 设 xMin 只挪位置、不改尺寸（Unity 是对边不动、width 跟着变）。
    ("RCM09 setting xMin translates instead of resizing", RECT_CPP, "::Rect Rect::WithXMin",
     "        out.x = v;\n        out.width = r.GetXMax() - v;", "        out.x = v;"),

    # 设 min 只挪位置、不改尺寸：对角就跟着飘出去了（RC26 要求 xMax 仍是 10）。
    ("RCM10 setting min translates instead of resizing", RECT_CPP, "::Rect Rect::WithMin",
     "        out.width = r.GetXMax() - min.x;", ""),

    # 设 center 忘了减半个尺寸。
    ("RCM12 setting center forgets the half-size offset", RECT_CPP, "::Rect Rect::WithCenter",
     "        out.x = center.x - r.width * 0.5f;\n        out.y = center.y - r.height * 0.5f;",
     "        out.x = center.x;\n        out.y = center.y;"),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ 只有源码门 RCW1 看得见：改成一个空格不影响任何运行期行为。
    ("RCM13 the Rect using-alias is renamed (it stops matching Unity's name)", RECT_H,
     "using Rect = RectT<float>;", "using Rect  = RectT<float>;"),

    # ⚠️ 只有源码门 RCW12 看得见：少了 .d.ts 声明不影响 C++，但用户工程里
    # `RectInt` 这个名字根本不存在。
    ("RCM14 the RectInt .d.ts declaration is gone", BINDING_CPP,
     '        "declare interface RectInt {\\n"\n', ""),
]

LOCK = os.path.join(BAK, "zengine_mutate_iv.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_iv.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".ivbak")
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
                with io.open(os.path.join(ROOT, ".mutate_iv.log"), "a", encoding="utf-8") as lg:
                    lg.write("%-64s %s\n" % (name, verdict))
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
