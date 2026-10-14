"""大规模真实样本评估：把样本量从 8 推到数百，看指标是否站得住。

**这个文件解决什么问题**
------------------------------------------------------------

`run_eval_lab.py` 只跑 8 个手写模板，它自己就写着"样本量小，无统计
意义"。八个样本上拿到 top1=100%，和三个样本上拿到 100% 没有本质区别：
置信区间宽到能塞进任何结论。

但**光把样本量堆上去是不够的** —— 上一版本脚本就是这么做的，它扩到
122 个样本却只报三个自造数字：有效样本率、top1、负样本误报。而
README 里被标为「最关键」的 honesty，以及 stability、
evidence_completeness、false_positive_rate，**一个都没出来**。

原因很简单：上一版压根没走 `zagent.eval`。它自己统计「崩溃了没」，
用"没崩"代替"克制" —— 这两个不是一回事（见下）。于是 122 个样本跑完，
README 定义的核心指标仍然只在 8 个样本上被测过。

本版把它接回 `zagent.eval.EvalReport`，让大规模样本和 8 样本跑同一套
指标定义。规模变了，指标的含义不能变。

**为什么负样本不能用「崩溃了没」判定**
------------------------------------------------------------

这是上一版最实的错误。实测：

    leak_01          crashed=False  定位 453  期望 453  -> 真命中
    race_01          crashed=False  定位 453  期望 453  -> 真命中

泄漏和竞争本来就**不该崩溃**：它们的证据是堆分配点 / 冲突现场，不是
异常。上一版看到"没崩"就记「克制正确」，把两个真命中当成了负样本上的
正确行为 —— 既虚增了 honesty 的分子，又把它们从 top1 的分子里扣掉了。

正确判据是「有没有拿到运行时证据」，即流水线是否产出了定位
（`run.report.location is not None`）。这与 `run_eval_lab.py` 的判据
一致，两个脚本对正负样本的定义必须相同，否则数字无法互证。

**负样本为什么也要有难度维度**
------------------------------------------------------------

只拿一个 clean_01 测 honesty，测的是「最干净的那种干净」：main 直接
返回、旁边没有长得像缺陷的代码。系统在这种样本上克制，说明不了它在
「干净但调用链很深、隔壁就是一段高度相似但有正确检查的死代码」时是否
还克制 —— 而后者才是诚实度真正会崩的地方。

所以负样本也按 depth / decoy 展开（见 zfault.generate_clean_variants），
让 honesty 也能按难度分组。

**ground truth 的独立性**
------------------------------------------------------------

expected_line 一律取 `lab.run(...).expected_line` —— 它由注入后写在磁盘
上的源码重算，与流水线无关。**绝不能取 report.location.line**，那是拿
报告自己的答案当期望值，永远 100%，比标签错位更隐蔽。

expected_path 则构造为 `_lab_work/<样本id>/f.cpp`。这是刻意的选择：
`score()` 用 `_normalize` 比对，它取路径最后两级目录，于是两侧都归一成
`<样本id>/f.cpp:行号`，格式对齐而不泄露答案 —— 路径只用于对齐比对格式，
行号仍是完全独立的 ground truth。

跑法：
    cd agentkit
    python -m examples.run_eval_lab_scale              # 默认每模板 6 变体
    python -m examples.run_eval_lab_scale --per 12
    python -m examples.run_eval_lab_scale --seed 1     # 换一批样本
    python -m examples.run_eval_lab_scale --repeat 1   # 关掉稳定性重跑（省一半时间）
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from zagent.eval import Sample, SampleSet, evaluate, score    # noqa: E402
from zagent.types import Report                              # noqa: E402
from zfault.patterns import all_patterns                     # noqa: E402
from zfault.variants import generate_clean_variants          # noqa: E402
from zfault.variants import generate_variants                # noqa: E402
from zlab.lab import Lab                                     # noqa: E402

# 工程内固定目录 —— 不用 tempfile.mkdtemp()，理由见 run_eval_lab.py 文件头。
WORKDIR = ROOT / "_lab_work"

# 变体 id 形如 `null_deref_01.d2y.s123`。用正则而不是 split('.')，
# 因为基础模板 id 里本身就有点号分隔的数字段。
_VARIANT_RE = re.compile(r"\.d(\d)([yn])\.s\d+$")


def _parse_variant_dims(vid: str) -> tuple[int, bool]:
    """从变体 id 反解 (depth, decoy)。非变体 id 返回 (0, False)。"""
    m = _VARIANT_RE.search(vid)
    if not m:
        return (0, False)
    return (int(m.group(1)), m.group(2) == "y")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=6, help="每个模板的变体数")
    ap.add_argument("--seed", type=int, default=20261014)
    ap.add_argument("--repeat", type=int, default=2,
                    help=">1 时额外测稳定性（每样本多跑 repeat 次）")
    ap.add_argument("--timeout", type=float, default=0, help="单样本超时秒数")
    a = ap.parse_args()

    lab = Lab(workdir=WORKDIR)
    if not lab.supported:
        print("本机不支持 zlab（需要 MSVC + Windows）")
        return 2

    # 正样本变体 + 负样本变体（负样本同样带难度维度）
    variants = generate_variants(per_pattern=a.per, seed=a.seed)
    negatives = generate_clean_variants(seed=a.seed)
    patterns = variants + negatives

    # id -> FaultPattern，run_fn 里按 id 取回
    by_id = {p.id: p for p in patterns}

    print()
    print("大规模真实样本评估（接 zagent.eval 指标）")
    print("  正样本变体 %d + 负样本变体 %d = %d 个" % (len(variants), len(negatives), len(patterns)))
    print("  每模板变体数 %d，seed %d，repeat %d" % (a.per, a.seed, a.repeat))
    print("  " + "-" * 62)

    # ================================================ 第一遍：实测标签
    #
    # 必须先跑一遍才能知道每个样本的 ground truth：注入会改变源码，行号
    # 以注入后的文件为准，不能靠模板里的原始行号猜。
    #
    # 这一遍同时决定正负样本归属 —— 判据是"是否拿到运行时证据"，
    # 不是"是否崩溃"。理由见文件头。
    print()
    print("第一遍：编译 / 崩溃 / 符号化，取真实根因行并标定正负样本 ...")
    truth: dict[str, tuple[str, int]] = {}      # id -> (gt_path, gt_line)
    evidence: dict[str, bool] = {}              # id -> 是否拿到运行时证据

    t0 = time.time()
    for i, p in enumerate(patterns, 1):
        try:
            run = lab.run(p)
        except Exception as e:                            # noqa: BLE001
            print("  ERROR %-34s %s" % (p.id, str(e)[:60]))
            continue
        has_ev = run.report is not None and run.report.location is not None
        evidence[p.id] = has_ev
        if has_ev:
            # 路径只用于对齐 _normalize 的比对格式，行号是独立 ground truth
            truth[p.id] = (str(WORKDIR / p.id / "f.cpp"), run.expected_line)
        else:
            truth[p.id] = ("", run.expected_line)
        if i % 20 == 0:
            print("  ... %d/%d" % (i, len(patterns)))
    print("  耗时 %.1f 秒" % (time.time() - t0))

    # ---- 装配样本集
    samples: list[Sample] = []
    for p in patterns:
        if p.id not in evidence:
            continue            # 跑挂了的不进样本集（下面单独记为异常）
        gt_path, gt_line = truth[p.id]
        neg = not evidence[p.id]
        depth, decoy = _parse_variant_dims(p.id)
        samples.append(Sample(
            id=p.id,
            ref=p.id,
            expected_path=gt_path,
            expected_line=None if neg else gt_line,
            negative=neg,
            note="%s depth=%d decoy=%s" % (p.category, depth, decoy),
        ))

    skipped = [p.id for p in patterns if p.id not in evidence]
    n_pos = sum(1 for s in samples if not s.negative)
    n_neg = sum(1 for s in samples if s.negative)
    print()
    print("  正样本 %d（拿到运行时证据），负样本 %d（无证据，应克制）" % (n_pos, n_neg))
    if skipped:
        print("  跑挂 %d 个，不进样本集：%s" % (len(skipped), skipped[:5]))

    sample_set = SampleSet(name="zlab-scale-variants", samples=samples)

    # ================================================ 第二遍：正式评估
    def run_fn(s: Sample):
        # report 可能为 None（降级路径，探测不到证据）—— evaluate 内部会
        # 访问 report.status，这里给一个明确表达"无结论"的空报告，
        # 而不是让它崩掉。与 run_eval_lab.py 保持一致。
        run = lab.run(by_id[s.id])
        if run.report is not None:
            return run.report
        return Report(status="failed", block_reason="no_runtime_evidence",
                      confidence=0.0, conclusions=[], unknowns=[
                          f"{s.id}: 无运行时证据（不崩/无栈），不予定位"])

    print()
    print("第二遍：正式评估 ...")
    t1 = time.time()
    report = evaluate(sample_set, run_fn, repeat=a.repeat)
    print("  耗时 %.1f 秒" % (time.time() - t1))
    print()
    print(report.render())

    # ================================================ 按难度分组
    #
    # 整体 100% 可能只是因为大部分样本都是 depth=0 的简单情形。分组报告
    # 才能看出能力在哪个边界上崩 —— 一个只在 depth=0 上 100% 的系统，
    # 和在任何深度都 100% 的系统，是完全不同的东西。
    #
    # 注意分组用的是**样本维度**而不是 agent 的输出，保证是独立观测。
    print()
    print("按调用深度分组（检测点与根因点的距离）")
    print("    depth    样本   命中   top1")

    # 复跑一次拿逐样本结果（evaluate 只留汇总和失败明细）
    per_case: dict[str, bool] = {}
    for s in samples:
        if s.negative:
            continue
        r = run_fn(s)
        per_case[s.id] = score(s, r).top1_hit

    by_d: dict[int, list[bool]] = {}
    for s in samples:
        if s.negative or s.id not in per_case:
            continue
        d, _ = _parse_variant_dims(s.id)
        by_d.setdefault(d, []).append(per_case[s.id])
    for d in sorted(by_d):
        v = by_d[d]
        print("    %-7d %5d %6d   %5.1f%%" % (d, len(v), sum(v), 100.0 * sum(v) / len(v)))

    print()
    print("按干扰项分组")
    print("    干扰    样本   命中   top1")
    by_k: dict[str, list[bool]] = {}
    for s in samples:
        if s.negative or s.id not in per_case:
            continue
        _, dec = _parse_variant_dims(s.id)
        by_k.setdefault("有" if dec else "无", []).append(per_case[s.id])
    for k in sorted(by_k):
        v = by_k[k]
        print("    %-6s %5d %6d   %5.1f%%" % (k, len(v), sum(v), 100.0 * sum(v) / len(v)))

    # 负样本也按难度分组 —— honesty 的边界同样要能看出来
    if n_neg:
        print()
        print("负样本按难度分组（诚实度 = 主动不下结论）")
        print("    depth   干扰   样本   克制")
        by_nd: dict[tuple[int, bool], list[bool]] = {}
        for s in samples:
            if not s.negative:
                continue
            d, dec = _parse_variant_dims(s.id)
            r = run_fn(s)
            by_nd.setdefault((d, dec), []).append(score(s, r).honest)
        for (d, dec) in sorted(by_nd):
            v = by_nd[(d, dec)]
            print("    %-6d %-5s %5d %6d" % (d, "有" if dec else "无", len(v), sum(v)))

    # ================================================ 失败清单
    if report.failures:
        print()
        print("失败样本 %d 个：" % len(report.failures))
        for f in report.failures[:15]:
            print("    %-34s %s" % (f.sample_id, f.failure_type))
        if len(report.failures) > 15:
            print("    ... 另有 %d 个" % (len(report.failures) - 15))

    # ================================================ 退出码
    #
    # 有失败就返回非 0。否则"失败样本被静默排除、剩下几个仍报 100%"
    # 会变成可接受的默认行为 —— 那正是拿过滤后的数字交差。
    print()
    if skipped:
        print("  跑挂 %d 个样本" % len(skipped))
        return 1
    if report.failures:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
