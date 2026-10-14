"""缺陷变体生成器：把 8 个手写模板扩成数百个**结构性不同**的样本。

为什么不能只是"改改变量名跑 200 次"
------------------------------------
如果变体只改标识符和常量，缺陷本质不变，200 个样本仍是同一道题 ——
统计意义是假的，置信区间宽度骗人。这正是上一轮合成样本 100% 给我们的
教训：数字好看，但它证明的东西和 8 个样本时一样少。

所以本模块的变体维度都作用在**定位难度**上，而不是表面文本：

  depth        检测点与根因点之间隔几层调用。层数越多，栈里有效帧越少，
               越依赖跨帧推理。
  decoy        是否插入结构相似但有正确检查的死代码。它专门制造
               "就近匹配" 陷阱：agent 若只做文本相似度，会命中死代码。

每个维度都真实改变"什么算答对"，因此样本之间是独立观测。

关键约束：ground truth 必须重算
-------------------------------
变体是改源码后重新生成的，行号必然漂移。上轮就栽在这里 —— 注入后行号
变了而标签没跟着变，agent 定位正确也被判 MISS。这类**标签失真**比算法
缺陷隐蔽得多：它让好算法看起来不行。

因此每个变体生成后都在**注入后的文本**上重算，算不出来就直接拒绝该
变体，绝不退回模板的原始行号。

可复现性：全部随机来自 random.Random(seed)，同种子必得同样本集。
"""

from __future__ import annotations

import random
import re
from dataclasses import replace
from typing import Iterable

from .patterns import FaultPattern, all_patterns

# 只在有明确根因标记的模板上生成变体。
# clean_01 是负样本，leak_01 不崩溃 —— 它们不参与变体，否则会稀释
# 正样本比例，让 top1 因为"样本变多"而不是"能力变强"上涨。
_VARIANT_EXCLUDE = {"clean_01", "leak_01"}

# 判据与 FaultPattern 共用同一常量，避免两处逻辑漂移导致标签失真
_FAULT_MARKER = "// " + "<FAULT>"


def _find_fault_line(src: str, anchor: str = "") -> int:
    """在给定文本里找根因标记行（1-based），找不到返回 -1。

    **anchor 必须一起参与判断。** oob_write_01 的源码里有两个一模一样的
    下溢标记：一个在有边界检查的 CopyRegion 里（死代码），一个在无检查的
    CopyRegionUnchecked 里。main 只调用后者。只看标记不看 anchor，就会
    命中死代码那一行 —— 标签错 11 行（39 vs 28），agent 定位到真正的
    下溢点也会被判 MISS。

    这正是 FaultPattern._marker_match 存在的原因，所以判据必须与它一致，
    不能另写一套：两处漂移的后果是标签静默错位，从指标上完全看不出来。
    """
    for i, line in enumerate(src.splitlines(), 1):
        if _FAULT_MARKER not in line:
            continue
        if anchor and anchor not in line:
            continue
        return i
    return -1


def _wrap_depth(src: str, depth: int, seed: int) -> str:
    """
    把 main 的调用链加厚 depth 层。

    做法：把 main 的**整个函数体**搬进最内层转发函数，外面套 depth 层
    无参转发。栈上因此多出 depth 个与根因无关的帧，检测点（栈顶）与
    根因点的距离被真实拉大 —— 不是靠改数字，而是靠改调用结构。

    **为什么搬整个 body 而不是抽一条 return 语句**

    抽语句的版本在这里栽了两次：
      - 转发层无参，但被转发的表达式引用 main 的 `argc` -> 编译失败
      - 改成带 `int argc` 后，`FillTable(count)` 里的 `count` 是 main 的
        局部变量，转发层照样引用不到 -> 还是编译失败
    两次都是 42 个变体里 20+ 个跑不起来，而失败样本被 eval 静默排除，
    剩下的仍是 100% —— 差点又一次拿过滤后的数字交差。

    搬整个 body 就彻底避开：局部变量随 body 一起进入转发层作用域，
    转发层不需要知道它被调用于什么。转发层一律无参，也就没有签名
    不匹配这回事。
    """
    if depth <= 0:
        return src

    rng = random.Random(seed)
    main_m = re.search(r"int main\s*\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    if not main_m:
        return src

    body = main_m.group(1)
    if not body.strip():
        return src

    rng_names = ["Dispatch", "Relay", "Forward"]
    rng_local = random.Random(seed)

    # 最内层：原 main 的整个 body 搬进来，局部变量留在作用域内。
    # 必须带 (int argc)：body 里常常引用 argc（如
    # `LoadAndApply(argc > 1 ? 100 : 99, ...)`），无参就引用不到。
    inner = "{}{}".format(rng_names[0], rng_local.randint(1, 999))
    inserted = [
        "static int {}(int argc) {{\n"
        "    // 转发层：与缺陷无关，作用是拉开检测点与根因点的栈距离\n"
        "{}"
        "\n}}".format(inner, body)
    ]
    chain = "{}(argc)".format(inner)

    # 外层：逐层透传 argc，不解释它
    for i in range(1, depth):
        fname = "{}{}".format(rng_names[i % len(rng_names)], rng_local.randint(1, 999))
        inserted.append(
            "static int {}(int argc) {{\n"
            "    // 转发层：与缺陷无关，作用是拉开检测点与根因点的栈距离\n"
            "    return {};\n"
            "}}".format(fname, chain)
        )
        chain = "{}(argc)".format(fname)

    # 新 main 把 argc 交进链首，它不再直接触碰缺陷调用。
    #
    # **必须写成多行、花括号独占一行。** 曾经压成单行
    # `int main(int argc, char**) { return X; }`，结果探针注入的
    # WINDOWS_INSTALL 被插到这一行之后 —— 落在 main 函数体外面，
    # 装了个空壳：编译能过、运行不崩，24 个变体里 16 个静默失效。
    # probe.inject_probe 是按 "{ 独占一行" 这个形态定位 main 体首行的，
    # 变体是后来者，得守这个契约。
    new_main = ("int main(int argc, char**)\n"
                "{\n"
                "    return " + chain + ";\n"
                "}\n")
    out = src[:main_m.start()] + "\n".join(inserted) + "\n\n" + new_main + src[main_m.end():]
    return out


def _add_decoy(src: str, seed: int) -> str:
    """
    插入一段结构相似但**有正确检查**的死代码。

    这是最阴的一种：死代码结构与缺陷相似、文本相似度高，但它从不执行。
    agent 若依赖文本相似度而非执行证据，会命中死代码。

    刻意**不给死代码加根因标记**：如果加了，ground truth 行号可能落到
    死代码上，于是 agent 正确定位到真实缺陷反而被判 MISS。标签必须唯一
    指向真实根因，干扰项只在"看起来像"的层面存在。
    """
    rng = random.Random(seed + 7919)
    decoy = (
        "// 干扰项：下面这段有正确的边界检查，且 main 不会调用它。\n"
        "// 存在意义是诱捕\"就近文本匹配\"式的定位 —— 它和缺陷代码高度相似，\n"
        "// 但执行证据永远不会指向这里。\n"
        "static int GuardedPath{}(int argc) {{\n"
        "    if (argc <= 0) {{\n"
        "        return -1;   // 正确检查：此处不可能越界\n"
        "    }}\n"
        "    return argc & 0x7;\n"
        "}}\n".format(rng.randint(1, 9999))
    )
    m = re.search(r"int main\s*\(", src)
    if not m:
        return src
    return src[:m.start()] + decoy + "\n" + src[m.start():]


def make_variant(
    p: FaultPattern,
    *,
    depth: int = 0,
    decoy: bool = False,
    seed: int = 0,
) -> FaultPattern | None:
    """
    基于模板 p 生成一个变体。失败返回 None（绝不返回行号可疑的对象）。

    返回 None 的常见原因：注入后找不到根因标记。此时若退回模板原行号，
    ground truth 就是错的 —— 宁可少一个样本。
    """
    src = p.source
    if _find_fault_line(src, p.root_cause_anchor) < 0:
        return None

    if depth:
        src = _wrap_depth(src, depth, seed)
    if decoy:
        src = _add_decoy(src, seed)

    # 在**注入后的文本**上重算，这是本函数存在的理由
    line = _find_fault_line(src, p.root_cause_anchor)
    if line < 0:
        return None

    vid = "{}.d{}{}.s{}".format(p.id, depth, "y" if decoy else "n", seed)
    return replace(
        p,
        id=vid,
        source=src,
        title="{} [depth={} decoy={}]".format(p.title, depth, decoy),
        notes=(p.notes + " " if p.notes else "")
              + "变体: depth={} decoy={} seed={}".format(depth, decoy, seed),
    )


def generate_variants(
    *,
    per_pattern: int = 6,
    seed: int = 20261014,
    base: Iterable[FaultPattern] | None = None,
) -> list[FaultPattern]:
    """
    生成变体集。每个基础模板出 per_pattern 个，维度确定性轮换。

    维度用轮换而非随机，是为了保证覆盖均衡：随机抽样在小样本下很容易
    把 depth=3 全漏掉，那样"深度是否影响准确率"就测不出来。
    """
    src_patterns = list(base) if base is not None else all_patterns()

    # 非确定性模板（race_01、overflow_01 的 deterministic=False）不生成
    # 变体：它们是否崩溃本身就随机，跑出来的"未崩溃"是样本性质而不是
    # 生成器失效，混进批量评估只会制造无法归因的噪声 —— 还会让有效样本
    # 率这个指标失真。它们仍作为原始模板参与 run_eval_lab。
    usable = [p for p in src_patterns
              if p.id not in _VARIANT_EXCLUDE and p.deterministic]

    combos = []
    for d in (0, 1, 2, 3):
        combos.append((d, False))
        combos.append((d, True))

    out = []
    for p in usable:
        for k in range(per_pattern):
            depth, decoy = combos[k % len(combos)]
            v = make_variant(p, depth=depth, decoy=decoy,
                             seed=seed + hash(p.id) % 1000 + k)
            if v is not None:
                out.append(v)
    return out


def corpus(base: Iterable[FaultPattern] | None = None) -> list[FaultPattern]:
    """变体 + 原始模板（含负样本）。负样本保留原样，不生成变体。"""
    base_list = list(base) if base is not None else all_patterns()
    variants = generate_variants(base=base_list)
    originals = [p for p in base_list if p.id in _VARIANT_EXCLUDE]
    return variants + originals