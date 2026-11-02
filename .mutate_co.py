"""Mutation harness for the `Color` / `ColorUtility` slice (#46).

Every function here is a handful of lines, so the interesting failures are not
"the algorithm is wrong" but "a *plausible* neighbouring algorithm was used
instead":

* `grayscale` as `(r+g+b)/3` instead of Rec.601 weights -- identical on white and
  black, and only different per-channel on the three primaries (CX0-CX2).
* `ToLinear` as the identity, or as plain `pow(v, 2.2)`: 0 and 1 are fixed points
  of every sRGB curve, so the endpoints prove nothing (CX7).
* The HTML string form reusing `::Color32`'s **truncation** instead of Unity's
  **rounding**: one bit off at 0.5 (0x80 vs 0x7F), invisible on a screenshot.
* The short hex form expanding with `*16` instead of `*17`: "#FFF" comes back as
  #F0F0F0 instead of white (CX23).
* A failed parse that keeps the caller's old colour instead of zeroing: the
  config file's typo then silently reuses the previous entry (CX28).

Two test/harness gaps the first round found:

* COM14 (the upper clamp on `CorrelatedColorTemperatureToRGB`) survived: the
  range check sampled 500/1000/20000/40000 K, but the upper clamp is only live
  just past t=66 (~6650 K, where the red branch still yields 256.9) -- 6500 K
  takes the "red is flat 255" branch and 10000 K has already decayed. CX35 is now
  a sweep over 500..40000 K in 50 K steps instead of four hand-picked points.
* COM17 was an invalid mutant: renaming `ColorParseResult` to
  `ColorParseResult_RENAMED` still contains `declare interface ColorParseResult`
  as a prefix, so the substring gate still hit. Now the whole block is removed.

Run:  py .mutate_co.py            (all)
      py .mutate_co.py COM01      (subset)
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = r"E:\Engine\ZEngine"

COLOR_H = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptColor\ScriptColor.h")
COLOR_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\ScriptColor\ScriptColor.cpp")
BINDING_CPP = os.path.join(ROOT, r"engine\Source\Runtime\Scripting\Binding\ScriptBinding.cpp")

PARITY_EXE = r"build\engine\Source\Runtime\Function\Framework\Test\Debug\UnityParitySmokeTest.exe"
COMP_EXE = r"build\engine\Source\Runtime\Scripting\ScriptVM\Test\Debug\ScriptComponentSmokeTest.exe"
BAK = r"C:\Users\zentiali\AppData\Local\Temp"

ENV = dict(os.environ)
for k in ("https_proxy", "http_proxy", "HTTPS_PROXY", "HTTP_PROXY"):
    ENV.pop(k, None)
ENV["PATH"] = r"E:\Engine\ZEngine\bin\Debug;" + ENV["PATH"]

# `ScriptColor.cpp` 的函数都在 `namespace Runtime { ... }` 里，所以函数尾是**4 空格**
# 的 `}`（默认标记正好），而命名空间自己的收尾带注释（`}  // namespace Runtime`），
# 顶格的 `"\n}\n"` 在这两个文件里**根本不存在** —— 第一轮 COM10-COM14 就是这么
# 报 "END OF FUNC NOT FOUND" 的。函数内部的 if / for 收尾是 8 空格，不会误停。
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
    # ---- ⚠️ 头条：看着合理的"另一个算法" -------------------------------------
    # 平均亮度。白与黑上**完全相同**，只有三个纯色分得开（0.299 / 0.587 / 0.114
    # vs 三条都是 0.333）。
    ("COM01 GetGrayscale averages the channels instead of weighting them (Rec.601)",
     COLOR_CPP, "float Color::GetGrayscale(",
     "        return c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;",
     "        return (c.r + c.g + c.b) / 3.0f;"),

    # 把 alpha 也算进亮度：半透明的红和纯红的"亮度"就不同了，而"按亮度自动选
    # 前景色"会在半透明背景上选错。
    ("COM02 GetGrayscale folds alpha into the luminance",
     COLOR_CPP, "float Color::GetGrayscale(",
     "        return c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;",
     "        return (c.r * 0.299f + c.g * 0.587f + c.b * 0.114f) * c.a;"),

    # maxColorComponent 把 alpha 也算进去：alpha 通常是最接近 1 的那个，于是
    # 几乎所有颜色都返回 1 —— 恰好是"看起来对"的那种错。
    ("COM03 GetMaxColorComponent includes alpha",
     COLOR_CPP, "float Color::GetMaxColorComponent(",
     "        float max = c.r;",
     "        float max = c.a;"),

    # ---- linear / gamma ------------------------------------------------------
    # 恒等变换。0 和 1 是所有 sRGB 曲线的不动点，所以"端点对"说明不了任何事 ——
    # 只有中灰 0.5 -> 0.214 那一条能抓住（CX7）。
    ("COM04 ToLinear is the identity (no sRGB decode at all)",
     COLOR_CPP, "::Color Color::ToLinear(",
     "        return Make(Mathf::GammaToLinearSpace(c.r), Mathf::GammaToLinearSpace(c.g),",
     "        return Make(c.r, c.g,"),

    # 只用 `pow(v, 2.2)`：0.5 -> 0.2176 而不是 0.2140。差 0.0036，肉眼完全看不出，
    # 但和 Unity 对不上（线性空间里做叠加时误差会积累）。
    ("COM05 ToLinear uses plain pow(v, 2.2) instead of the piecewise sRGB curve",
     COLOR_CPP, "::Color Color::ToLinear(",
     "        return Make(Mathf::GammaToLinearSpace(c.r), Mathf::GammaToLinearSpace(c.g),",
     "        return Make(std::pow(c.r, 2.2f), std::pow(c.g, 2.2f),"),

    # alpha 也过一遍曲线：同一个颜色的不同透明度会得到不同的 RGB，"改了 alpha
    # 颜色也跟着变"。
    ("COM06 ToLinear converts alpha too",
     COLOR_CPP, "::Color Color::ToLinear(",
     "                    Mathf::GammaToLinearSpace(c.b), c.a);",
     "                    Mathf::GammaToLinearSpace(c.b), Mathf::GammaToLinearSpace(c.a));"),

    # ---- ToString -------------------------------------------------------------
    # 打成 0..255 的整数：和 Color32.ToString 混了。
    ("COM07 Color.ToString prints 0..255 bytes instead of 0..1 floats",
     COLOR_CPP, "std::string Color::ToString(",
     '        std::snprintf(buffer, sizeof(buffer), "RGBA(%.3f, %.3f, %.3f, %.3f)", '
     "static_cast<double>(c.r),",
     '        std::snprintf(buffer, sizeof(buffer), "RGBA(%d, %d, %d, %d)", '
     "static_cast<int>(c.r * 255.0f),"),

    # ---- ColorUtility.ToHtmlString* -------------------------------------------
    # ⚠️ 复用 `::Color32(c)` 的**截断**：0.5 -> 127 = 0x7F，而 Unity 走的是
    # RoundToInt -> 128 = 0x80。只差一位，截图上看不出来，配色表却对不上。
    ("COM08 ToHtmlStringRGB truncates like Color32 instead of rounding like Unity",
     COLOR_CPP, None,
     "            return static_cast<int>(std::floor(v * 255.0f + 0.5f));",
     "            return static_cast<int>(v * 255.0f);"),

    # RGB 版把 alpha 也拼进去：同一个颜色在两条路上得到两个串。
    ("COM09 ToHtmlStringRGB appends alpha (it is supposed to drop it)",
     COLOR_CPP, "std::string ColorUtility::ToHtmlStringRGB(",
     '        std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X", ChannelToByte(c.r), '
     "ChannelToByte(c.g),\n                      ChannelToByte(c.b));",
     '        std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X", ChannelToByte(c.r), '
     "ChannelToByte(c.g),\n                      ChannelToByte(c.b), ChannelToByte(c.a));"),

    # ---- ColorUtility.TryParseHtmlString ---------------------------------------
    # ⚠️ 缩写每位 `*16` 而不是 `*17`：'#FFF' 变成 #F0F0F0 而不是纯白。每个通道差
    # 15/255，肉眼几乎看不出。
    ("COM10 the short hex form expands with *16 instead of *17 ('#FFF' is not white)",
     COLOR_CPP, "ColorParseResult ColorUtility::TryParseHtmlString(",
     "                components[i] = HexValue(html[begin + static_cast<std::size_t>(i)]) * 17;",
     "                components[i] = HexValue(html[begin + static_cast<std::size_t>(i)]) * 16;",
),

    # 6 位时 alpha 补 0：解析出来的颜色**全透明**，UI 直接消失。
    ("COM11 a 6-digit string defaults alpha to 0 instead of 1",
     COLOR_CPP, "ColorParseResult ColorUtility::TryParseHtmlString(",
     "            if (pairs == 3)\n            {\n                components[3] = 255;\n            }",
     "            if (pairs == 3)\n            {\n                components[3] = 0;\n            }",
),

    # 长度检查取消：空串也被当成合法（4 位分支 pairs = 0，什么都不读）。
    ("COM12 TryParseHtmlString accepts any length",
     COLOR_CPP, "ColorParseResult ColorUtility::TryParseHtmlString(",
     "        if (length != 3 && length != 4 && length != 6 && length != 8)",
     "        if (false)",
),

    # 失败时不清零：调用方会当成"解析成功"拿到上一个颜色 —— 配置表里一个拼错的
    # 色值于是变成"悄悄用了上一条"，比"看不见"难查得多。
    ("COM13 a rejected parse leaves a stale colour instead of all-zero",
     COLOR_CPP, "ColorParseResult ColorUtility::TryParseHtmlString(",
     "        result.color = Make(0.0f, 0.0f, 0.0f, 0.0f);",
     "        result.color = Make(0.5f, 0.25f, 0.75f, 1.0f);",
),

    # ---- CorrelatedColorTemperatureToRGB ---------------------------------------
    # 去掉上夹：t 很大时幂函数会溢出 255。
    ("COM14 CorrelatedColorTemperatureToRGB does not clamp its channels",
     COLOR_CPP, "::Color Color::CorrelatedColorTemperatureToRGB(",
     "            if (v > 255.0f) v = 255.0f;",
     "            ;",
),

    # 去掉下夹：t 很小时 log(t) 是负的（500 K 的绿通道算出来是 -1.03）。
    ("COM15 CorrelatedColorTemperatureToRGB does not clamp negative channels",
     COLOR_CPP, "::Color Color::CorrelatedColorTemperatureToRGB(",
     "            if (v < 0.0f) v = 0.0f;",
     "            ;"),

    # ---- 发布面 ---------------------------------------------------------------
    # ⚠️ 只有源码门看得见：C++ 照常工作，脚本里那个名字根本不存在 —— 而"方法
    # 不存在"在运行时是静默无事发生。
    ("COM16 the ToHtmlStringRGB ZFUNCTION is dropped (C++ keeps working, the script name is gone)",
     COLOR_H,
     "        ZFUNCTION()\n        static std::string ToHtmlStringRGB(const ::Color& c);",
     "        static std::string ToHtmlStringRGB(const ::Color& c);"),

    # 值类型的 d.ts 声明漏了：C++ 编译得过、运行时也跑得通，只有 tsc 会报
    # `Property 'valid' does not exist` —— 而 tsc 不在本 harness 里跑，所以这条
    # 只能靠源码门 CXW12。
    # ⚠️ 只把名字改成 `..._RENAMED` 是**无效变异**："declare interface
    # ColorParseResult" 仍然是它的前缀，CountOccurrences 照样命中（第一轮就是
    # 这么漏的）。要整块摘掉。
    ("COM17 ColorParseResult is missing from the generated .d.ts",
     BINDING_CPP,
     '        "declare interface ColorParseResult {\\n"\n'
     '        "    valid: boolean;\\n"\n'
     '        "    color: Color;\\n"\n'
     '        "}\\n"',
     '        "// (mutated away)\\n"'),
]

LOCK = os.path.join(BAK, "zengine_mutate_co.lock")


def main():
    if os.path.exists(LOCK):
        raise SystemExit("another .mutate_co.py is already running (%s)" % LOCK)
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
            bak = os.path.join(BAK, os.path.basename(path) + ".cobak")
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
                with io.open(os.path.join(ROOT, ".mutate_co.log"), "a", encoding="utf-8") as lg:
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
