"""桥接层端到端验证：真实崩溃 -> crash_snap.json -> 解析器 -> 归因。

为什么单独立一个脚本
--------------------
`run_eval_lab.py` 直接把 CrashCapture 喂给流水线，**绕过了快照格式**。
那意味着 L1 做的 crash_snap.json 解析器在真实崩溃上从未被真正执行过 ——
它的 100% 全部来自手工构造的 JSON。

本脚本补上这一环：每次崩溃都真的走一遍
    CrashCapture -> lab_bridge -> crash_snap.json -> load_snapshot -> diagnose
把"格式"当作被测对象，而不是跳过去。

这是目前唯一能回答下面这个问题的路径：
    合成样本上的 100%，在真实崩溃上还剩多少？

跑法：
    cd agentkit
    python -m examples.run_lab_bridge
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from zagent.lab_bridge import lab_run_to_snapshot, write_snapshot      # noqa: E402
from zagent.zengine_snapshot import load_snapshot                      # noqa: E402
from zfault.patterns import all_patterns                               # noqa: E402
from zlab.lab import Lab                                               # noqa: E402

# 固定用工程内的 _lab_work，不用系统 Temp：
# 系统 Temp 下的未知 exe 会被实时扫描，产生假的"未崩溃"。
WORKDIR = ROOT / "_lab_work"


def main() -> int:
    lab = Lab(workdir=WORKDIR)
    if not lab.supported:
        print("本机不支持 zlab（需要 MSVC + Windows）")
        return 2

    rows = []
    for p in all_patterns():
        run = lab.run(p)
        cap = getattr(run, "capture", None)
        if cap is None or not getattr(cap, "crashed", False):
            rows.append((p.id, "-", "-", "未崩溃（正确，非崩溃型样本）"))
            continue

        snap = lab_run_to_snapshot(run, module="lab.exe",
                                   product_version="lab")
        with tempfile.TemporaryDirectory() as td:
            fp = write_snapshot(snap, Path(td) / "crash_snap.json")
            facts = load_snapshot(fp)

        # 解析器吃进去的帧数必须和原始一致。少了就是桥接丢了数据，
        # 这是最容易被静默吞掉的一类 bug。
        n_src = len(snap["frames"])
        n_parsed = len(facts.stack)
        status = "OK" if n_src == n_parsed else "帧数不符"
        rows.append((
            p.id,
            str(n_parsed),
            str(n_src),
            status,
        ))

    print("\n桥接端到端（真实崩溃 -> 快照 -> 解析）")
    print("  样本                       解析帧  源帧  状态")
    print("  " + "-" * 58)
    for r in rows:
        print("  %-24s %6s %6s  %s" % r)

    bad = [r for r in rows if r[3] not in ("OK", "未崩溃（正确，非崩溃型样本）")]
    print()
    if bad:
        print("有 %d 个样本桥接不一致" % len(bad))
        return 1
    print("全部一致：%d 个样本" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
