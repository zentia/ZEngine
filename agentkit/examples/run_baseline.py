"""
基线 eval：把实验台的样本接进 zagent 的 eval 框架，产出真实数字。

为什么叫"基线"：它衡量的是当前（规则驱动、无 LLM）定位能力的真实水位。
之后任何改进都要跟这个数字比 —— 没有基线就无法回答"改了是不是变好了"。
"""

from __future__ import annotations

import json
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, r"E:\Engine\ZEngine\agentkit")

from zagent.eval import EvalReport, Sample, SampleSet, evaluate
from zfault.patterns import all_patterns
from zlab.lab import Lab
from zlab.probe import inject_probe


def build_samples(lab: Lab) -> SampleSet:
    """
    把故障模式转成 eval 样本。

    ground truth 用 lab.run 重算过的行（注入后），
    不是原始 pattern 的行号 —— 详见 lab.run 的说明。
    """
    ss = SampleSet(name="zfault-v1")
    for p in all_patterns():
        # 不崩溃的模式作为负样本：测的是"会不会瞎给结论"
        negative = p.category in ("leak", "race", "use_after_free")
        ss.samples.append(Sample(
            id=p.id,
            ref=p.id,
            expected_path="f.cpp",
            expected_line=-1,      # 占位，run 时用真实值替换
            negative=negative,
            note=f"{p.category} / {p.detect_hint[:60]}",
        ))
    return ss


def _root_func(injected_source: str, p) -> str:
    """注入后源码里 <FAULT> 所属的函数名。

    用 pattern.root_cause_line_in 定位，与 ground truth 共用 anchor 判据 ——
    否则多标记样本会取到错误函数（如 oob_write 的死代码 CopyRegion）。
    """
    lines = injected_source.splitlines()
    target = p.root_cause_line_in(injected_source)
    if target <= 0:
        return ""
    for j in range(target - 1, 0, -1):
        line = lines[j - 1]
        m = re.match(r"^[\w:<>*&\s]+\**\s+(\w+)\s*\(", line)
        if m and not line.strip().startswith(("//", "#", "return", "if", "for")):
            return m.group(1)
    return ""


def _located_func(run) -> str:
    """流水线定位到的函数名。"""
    if not (run.report and run.report.location):
        return ""
    sym = run.report.location.symbol or ""
    # 去参数：Evaluate(int, int) -> Evaluate
    return sym.split("(")[0].strip()


def main() -> int:
    # 工作目录固定在工程内，不用 tempfile.mkdtemp()。
    #
    # 实测：放在系统 Temp 目录下的 exe 首次运行会被防病毒实时扫描挂住
    # （30 秒不崩溃也不退出），导致样本被记成"未崩溃"，产生假的 MISS。
    # 这类环境噪声会让基线数字失真，掩盖真实能力。
    workdir = Path(__file__).resolve().parent.parent / "_lab_work"
    workdir.mkdir(parents=True, exist_ok=True)
    lab = Lab(workdir=workdir)
    if not lab.supported:
        print("无可用编译器，跳过")
        return 1

    rows = []
    # 跑完后用真实重算值回填，再判断命中
    for p in all_patterns():
        run = lab.run(p)
        row = {
            "id": p.id,
            "category": p.category,
            "expected_line": run.expected_line,
            "located_line": run.located_line,
            "hit": run.hit,
            "off_by": run.off_by,
            "crashed": run.capture.crashed,
            "signal": run.capture.signal,
            "nframes": len(run.capture.frames),
            "dropped": run.capture.frames_dropped,
            "expected_func": _root_func(inject_probe(p.source), p),
            "located_func": _located_func(run),
            "confidence": round(run.report.confidence, 2) if run.report else 0.0,
            "status": run.report.status if run.report else "no_report",
            "notes": list(run.capture.notes) + list(run.notes),
        }
        rows.append(row)

    # --- 汇总 ---
    crashable = [r for r in rows if r["crashed"]]
    located = [r for r in rows if r["located_line"] is not None]
    hits = [r for r in rows if r["hit"]]

    print("=" * 76)
    print(f"{'样本':<22}{'类别':<16}{'期望':>5}{'定位':>6}{'偏差':>6}{'帧数':>5}  结果")
    print("-" * 76)
    for r in rows:
        loc = r["located_line"] if r["located_line"] is not None else "-"
        off = r["off_by"] if r["off_by"] is not None else "-"
        mark = "HIT " if r["hit"] else ("LOC " if r["located_line"] else "MISS")
        print(f"{r['id']:<22}{r['category']:<16}{r['expected_line']:>5}"
              f"{str(loc):>6}{str(off):>6}{r['nframes']:>5}  {mark}")
    print("-" * 76)

    n = len(rows)
    crashable = [r for r in rows if r["crashed"]]
    located = [r for r in rows if r["located_line"] is not None]
    hits = [r for r in rows if r["hit"]]
    # 函数级命中：定位到的函数 == 根因所在函数。
    # 这比行级更宽容，也更贴近实用 —— 真实排查里"定位到哪个函数"
    # 往往已经解决了大半问题，而精确行号在"检测点≠破坏点"时本就不该强求。
    func_hits = [r for r in rows
                 if r["located_func"] and r["located_func"] == r["expected_func"]]

    print(f"样本总数        {n}")
    print(f"成功崩溃        {len(crashable)}/{n}")
    print(f"能定位到行      {len(located)}/{n}   ({len(located)/n:.0%})")
    print(f"命中根因行      {len(hits)}/{n}   ({len(hits)/n:.0%})")
    print(f"命中根因函数    {len(func_hits)}/{n}   ({len(func_hits)/n:.0%})")
    if located:
        offs = sorted(r["off_by"] for r in located if r["off_by"] is not None)
        print(f"定位偏差中位数  {offs[len(offs)//2]}")
    print("=" * 76)

    out = Path(r"G:\OSGame\doc\_zfl_stage\baseline.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n明细已写入 {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
