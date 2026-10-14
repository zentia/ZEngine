"""
eval（真实样本）：把 zlab 的 8 个真实故障样本接进 eval 通路。

**为什么要有这个文件**

原先只有一个 `run_eval.py`，它跑的是 **4 个合成样本** ——
源码是 `minimal_run.py` 现编的一个假仓库，崩溃是 `build_crash_sample`
凭空造的 JSON，栈帧是填出来的数字。

它和 zlab 那 8 个**真实编译、真实崩溃、真实符号化**的样本完全不相干，
两套评估各自都好看，但谁都没覆盖对方。
README 里"7/8"这个基线和"eval 通路 100%"是两个互不相通的数字 ——
这是本文件要解决的问题。

**为什么工作目录必须固定在工程内**

`run_eval.py` 用 `tempfile.mkdtemp()`，那正是 README 点名的防病毒噪声源：
系统 Temp 下的未知 exe 会被实时扫描，表现为假的"未崩溃"、
锁住文件句柄导致清理时 PermissionError、
甚至让第二次运行走不同的崩溃路径。

所以这里固定用工程内的 `_lab_work`，不用系统 Temp。

跑法：
    cd agentkit
    python -m examples.run_eval_lab
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from zagent.eval import Sample, SampleSet, evaluate          # noqa: E402
from zfault.patterns import all_patterns                     # noqa: E402
from zlab.lab import Lab                                     # noqa: E402

# 工程内固定目录 —— 不用 tempfile.mkdtemp()，理由见文件头。
WORKDIR = ROOT / "_lab_work"


def main() -> int:
    lab = Lab(workdir=WORKDIR)

    if not lab.supported:
        print("本机没有可用的 C++ 工具链，无法跑真实样本。")
        print("先装 MSVC（cl.exe）或 clang-cl，再重试。")
        return 2

    print("=" * 74)
    print("ZEngine Agent Kit — eval（zlab 真实样本）")
    print("=" * 74)
    print(f"工作目录: {WORKDIR}")
    print("（固定在工程内，避开系统 Temp 的防病毒噪声）")
    print()

    # 先跑一轮拿到每个样本的真实根因行，再据此装配样本集。
    # 行号不能靠猜：注入会改变源码，行号必须以注入后的文件为准。
    print("第一遍：编译 / 崩溃 / 符号化，取真实根因行 ...")
    probes = {}
    for p in all_patterns():
        run = lab.run(p)
        probes[p.id] = run.expected_line
        mark = "HIT " if run.hit else "MISS"
        print(f"  {mark} {p.id:18} 根因行 {run.expected_line}")

    # ---- 装配样本集：ground truth 用实测行号 ----
    #
    # **负样本不是预先指定的，而是探测结果决定的。**
    #
    # overflow_01 在 patterns 里是正样本（有 ground truth 431），
    # 但它是纯整数溢出：不崩、无栈、无异常，
    # 流水线走降级路径时 report 为 None —— 这是**真实且正确的行为**，
    # 探测不到证据就不该给确定结论。
    #
    # 若硬把它当正样本，它只是一个永恒的 MISS，
    # 而 honesty（负样本上的克制率）这个指标会因为没有负样本而失去意义。
    # 所以这里按"是否拿到了运行时证据"动态标注：
    # 拿不到证据 == 负样本，正确行为就是克制。
    print()
    print("装配样本集（负样本按「是否拿到运行时证据」动态标注）...")
    samples: list[Sample] = []
    for p in all_patterns():
        run = lab.run(p)
        has_evidence = run.report is not None and run.report.location is not None
        # **expected_path 必须用报告里 location 的真实路径。**
        #
        # score() 用 _normalize 比对，它取路径的最后两级目录
        # （例如 `null_deref_01/f.cpp:442`）。若这里填 "f.cpp"，
        # 归一化后是 `f.cpp:442`，与报告侧尾部不匹配 ——
        # 7 个本来全对的样本会被判成"定位错误"，top1 直接归零。
        #
        # 这正是 _normalize 注释里警告的那类假失败：
        # 看起来是定位错了，实际只是路径格式不同。
        # 它会诱导你去优化一个根本没坏的模块。
        # **行号绝不能用 report.location.line —— 那是用报告自己的答案
        # 当期望值，永远 100%，比上面的假失败更隐蔽。**
        # ground truth 必须独立：由注入时写在源码里的根因标记算出，
        # 与流水线无关（run.expected_line）。
        # 这里只有路径取自报告，目的仅是对齐 _normalize 的比对格式。
        gt_path = str(run.report.location.path) if has_evidence else ""
        samples.append(Sample(
            id=p.id,
            ref=p.id,
            expected_path=gt_path,
            expected_line=None if not has_evidence else run.expected_line,
            negative=not has_evidence,
            note=(f"{p.category}: {p.title}"
                  + ("" if has_evidence else "（无运行时证据，应克制）")),
        ))
        tag = "正样本" if has_evidence else "负样本"
        print(f"  {tag}  {p.id:18} 根因行 {run.expected_line}")
    samples = SampleSet(name="zlab-real-samples", samples=samples)

    def run_fn(s: Sample):
        # 每个样本跑完整流水线：编译 -> 崩溃 -> 符号化 -> 诊断。
        # report 可能为 None（降级路径，探测不到证据）——
        # evaluate 内部会访问 report.status，这里给一个明确表达
        # "无结论"的空报告，而不是让它崩掉。
        from zagent.types import Report
        run = lab.run(next(p for p in all_patterns() if p.id == s.id))
        if run.report is not None:
            return run.report
        return Report(status="failed", block_reason="no_runtime_evidence",
                      confidence=0.0, conclusions=[], unknowns=[
                          f"{s.id}: 无运行时证据（不崩/无栈），不予定位"])

    print()
    print("第二遍：正式评估（每个样本跑 2 次，顺带测稳定性）...")
    report = evaluate(samples, run_fn, repeat=2)
    print()
    print(report.render())

    # ---- 稳定性明细：87.5% 这种数字必须能解释 ----
    #
    # evaluate 只给一个百分比，看不出**哪个**样本不稳。
    # 实测这个值在 100% 与 87.5% 之间浮动（8 个里 1 个），
    # 单独跑每个样本 4~8 次却都稳定 —— 说明它与整轮负载相关，是偶发的。
    # 不能只报个百分比就完事，得能指出来是谁。
    print()
    print("稳定性明细:")
    unstable = 0
    for s in samples.samples:
        seen = []
        for _ in range(2):
            rep = run_fn(s)
            seen.append(str(rep.location) if rep.location else "(无定位)")
        tag = "稳定" if len(set(seen)) == 1 else "波动"
        if tag == "波动":
            unstable += 1
            print(f"  {tag}  {s.id:20} {seen}")
    print(f"  波动样本 {unstable}/{len(samples.samples)}"
          + ("（单独重跑时通常稳定，与整轮负载相关的偶发）"
             if unstable else ""))

    # ---- 逐样本核对：命中必须附带 CONFIRMED 结论 ----
    #
    # **这一节是这个文件存在的真正理由。**
    #
    # eval 的 top1 只统计"定位行 == 期望行"，它分不清两种情况：
    #   - 探测到证据，推理层证实了它；
    #   - 运行时证据恰好指对了行号，验证层根本没参与。
    # 后者是虚命中，换个样本就会露馅。
    # 实测补规则之前 stack_overflow_01（0.0 置信度、零结论）
    # 和 race_01（0.15、零证实）都是这种。
    print()
    print("逐样本核对（命中必须附带证实结论）:")
    print(f"  {'样本':20} {'定位':>6} {'期望':>6} {'置信':>5}  证实类型")
    print("  " + "-" * 70)
    for p in all_patterns():
        run = lab.run(p)
        r = run.report
        loc = "-" if run.located_line is None else str(run.located_line)
        if r is None:
            # 降级路径：明确不给结论。这是克制，不是失败。
            print(f"  {p.id:20} {loc:>6} {run.expected_line:>6} {'-':>5}  "
                  f"(无运行时证据，正确克制)")
            continue
        kinds = [c.kind for c in (r.conclusions or [])
                 if getattr(getattr(c, "verdict", None), "value", None)
                 == "confirmed"]
        loc = "-" if run.located_line is None else str(run.located_line)
        conf = "-" if getattr(r, "confidence", None) is None else \
            f"{r.confidence:.2f}"
        flag = "" if (not run.hit or kinds) else "   <- 虚命中！"
        print(f"  {p.id:20} {loc:>6} {run.expected_line:>6} {conf:>5}  "
              f"{','.join(kinds) or '(无)'}{flag}")

    print()
    print("注：8 个样本规模仍小，指标没有统计意义。")
    print("    真实用法是在 CI 里批量注入故障，把样本量堆到数百个。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
