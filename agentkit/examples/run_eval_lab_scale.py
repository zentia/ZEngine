"""大规模真实样本评估：把样本量从 8 推到数百，看指标是否站得住。

为什么要有这个脚本
------------------
`run_eval_lab.py` 只跑 8 个手写模板，它自己就写着"样本量小，无统计
意义"。八个样本上拿到 top1=100%，和三个样本上拿到 100% 没有本质区别：
置信区间宽到能塞进任何结论。

本脚本用 zfault.variants 把模板扩成数百个**结构性不同**的样本（不是
同一道题换名字），然后跑完整归因，并**按维度分组**报告。

分组报告才是重点：整体 100% 可能只是因为大部分样本都是 depth=0 的简单
情形。按 depth / decoy 拆开，才能看出能力到底在哪个边界上崩。一个只在
depth=0 上 100% 的系统，和在任何深度都 100% 的系统，是完全不同的东西。

跑法：
    cd agentkit
    python -m examples.run_eval_lab_scale              # 默认每模板 6 变体
    python -m examples.run_eval_lab_scale --per 12
    python -m examples.run_eval_lab_scale --seed 1     # 换一批样本
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from zfault.patterns import all_patterns                    # noqa: E402
from zfault.variants import corpus as build_corpus          # noqa: E402
from zfault.variants import generate_variants               # noqa: E402
from zlab.lab import Lab                                    # noqa: E402

WORKDIR = ROOT / "_lab_work"


def _parse_variant_dims(vid: str) -> tuple[str, int, bool]:
    """从变体 id 反解维度。

    变体 id 形如 `null_deref_01.d2y.s123`。用正则而不是 split('.')，
    因为基础模板 id 里本身就有点号分隔的数字段。
    """
    import re
    m = re.search(r"\.d(\d)([yn])\.s\d+$", vid)
    if not m:
        return ("base", 0, False)
    return ("variant", int(m.group(1)), m.group(2) == "y")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=6, help="每个模板的变体数")
    ap.add_argument("--seed", type=int, default=20261014)
    ap.add_argument("--timeout", type=float, default=0, help="单样本超时秒数")
    a = ap.parse_args()

    lab = Lab(workdir=WORKDIR)
    if not lab.supported:
        print("本机不支持 zlab（需要 MSVC + Windows）")
        return 2

    variants = generate_variants(per_pattern=a.per, seed=a.seed)
    negatives = [p for p in all_patterns()
                 if p.id in ("clean_01", "leak_01")]
    samples = variants + negatives

    print("\n大规模真实样本评估")
    print("  变体 %d + 负样本 %d = %d 个样本" % (len(variants), len(negatives), len(samples)))
    print("  每模板变体数 %d，seed %d" % (a.per, a.seed))
    print("  " + "-" * 62)

    rows = []
    t0 = time.time()
    for i, p in enumerate(samples, 1):
        try:
            run = lab.run(p)
        except Exception as e:                       # noqa: BLE001
            rows.append((p.id, "ERROR", "", str(e)[:60]))
            continue
        cap = getattr(run, "capture", None)

        kind, depth, decoy = _parse_variant_dims(p.id)

        if kind == "base":
            # 负样本：不崩溃才是对的。若它给出高置信结论 = 误报。
            crashed = bool(cap and getattr(cap, "crashed", False))
            verdict = "误报" if crashed else "克制正确"
            rows.append((p.id, "负样本", "-", verdict))
            continue

        if not cap or not getattr(cap, "crashed", False):
            # 变体没崩 = 生成失败，不是"agent 没查出来"。必须显式计入
            # 并让退出码非 0：否则失败样本被静默排除，剩下几个仍报 100%，
            # 就成了拿过滤后的数字交差。这正是上一轮合成样本的翻版。
            rows.append((p.id, "未崩溃", "-", "变体失效"))
            continue

        hit = bool(getattr(run, "hit", False))
        line = getattr(run, "located_line", 0)
        truth = getattr(run, "truth_line", 0)
        rows.append((p.id,
                     "命中" if hit else "MISS",
                     "%s/%s" % (line, truth),
                     "d%d%s" % (depth, "y" if decoy else "n")))
        if i % 10 == 0:
            print("  ... %d/%d" % (i, len(samples)))

    dt = time.time() - t0

    # ---- 汇总
    pos = [r for r in rows if r[1] in ("命中", "MISS")]
    err = [r for r in rows if r[1] == "ERROR"]
    uncrashed = [r for r in rows if r[1] == "未崩溃"]
    neg = [r for r in rows if r[1] == "负样本"]

    hits = sum(1 for r in pos if r[1] == "命中")
    total_pos = len(pos)

    print()
    # 分母用**全部变体**而不是"成功跑起来的那些"。
    # 用成功数做分母，生成器越烂指标越好看 —— 26 个跑不起来时top1 仍是
    # 100%，那个数字是过滤出来的，不是测出来的。
    print("  有效样本率 %d/%d = %.1f%%  （失效的是生成器问题，不是归因问题）"
          % (total_pos, len(variants),
             100.0 * total_pos / len(variants) if variants else 0.0))
    print("  正样本 %d，命中 %d  -> top1 = %.1f%%"
          % (total_pos, hits, 100.0 * hits / total_pos if total_pos else 0.0))
    print("  负样本 %d，误报 %d" % (len(neg), sum(1 for r in neg if r[3] == "误报")))
    if err:
        print("  异常 %d（见下）" % len(err))
    if uncrashed:
        print("  变体失效 %d（注入后未能崩溃，属生成器问题）" % len(uncrashed))
    print("  耗时 %.1f 秒" % dt)

    # ---- 按 depth 分组：这才是重点
    print()
    print("  按调用深度分组（检测点与根因点的距离）")
    print("    depth   样本   命中   top1")
    by_d: dict[int, list] = {}
    for r in pos:
        d = int(r[3][1]) if r[3][1].isdigit() else 0
        by_d.setdefault(d, []).append(r[1] == "命中")
    for d in sorted(by_d):
        v = by_d[d]
        print("    %-6d %5d %6d   %5.1f%%"
              % (d, len(v), sum(v), 100.0 * sum(v) / len(v)))

    # ---- 按 decoy 分组
    print()
    print("  按干扰项分组")
    print("    干扰   样本   命中   top1")
    by_k: dict[str, list] = {}
    for r in pos:
        k = "有" if r[3].endswith("y") else "无"
        by_k.setdefault(k, []).append(r[1] == "命中")
    for k in sorted(by_k):
        v = by_k[k]
        print("    %-5s %5d %6d   %5.1f%%"
              % (k, len(v), sum(v), 100.0 * sum(v) / len(v)))

    # ---- 失败清单
    bad = [r for r in rows if r[1] in ("MISS", "ERROR", "未崩溃")]
    if bad:
        print()
        print("  失败样本 %d 个：" % len(bad))
        for r in bad[:15]:
            print("    %-32s %-8s %-14s %s" % r)
        if len(bad) > 15:
            print("    ... 另有 %d 个" % (len(bad) - 15))

    print()
    if err or uncrashed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
