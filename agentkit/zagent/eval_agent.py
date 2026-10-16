"""
Agent 级 eval：让「跑 agent」和「跑流水线」产出**可直接比较**的数字。

为什么必须复用既有的 eval 而不是另起一套
----------------------------------------
`eval.py` 里已经有 `score()` / `evaluate()` / `EvalReport`，指标口径也定好了
（top1 / top3 / honesty / evidence）。如果 agent 另配一套指标，就会出现
两个数字：一个 100%、一个 87%，然后谁也说不清差异来自"agent 更差"还是
"尺子不一样"。

**eval 里的假差异比真差异更危险** —— 它让你去优化一个根本没坏的东西。
`eval.py` 里 `_normalize()` 那条注释讲的正是这个坑（路径格式不同被判成
定位错误）。这里同理：只要让 agent 也产出一个 `Report`，就能用同一把尺
子量，差异就只可能来自 agent 本身。

所以设计是：`diagnose_crash` 工具把 `Report` 对象放进 `ToolResult.data`，
本模块把它取出来交回既有的 `evaluate()`。指标口径一个字不改。

新增的指标只有"agent 特有"的那几个（步数、工具错误率、停滞率）——
它们衡量的是**过程**，不是结论质量。结论质量仍由原指标负责。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

from .eval import CaseResult, EvalReport, Sample, SampleSet, evaluate
from .loop import Action, RunOutcome, Turn
from .types import Report


# ---------------------------------------------------------------- 单次运行

@dataclass
class AgentRunStats:
    """一轮 agent 运行的过程指标。"""

    sample_id: str = ""
    status: str = ""            # done / stalled / max_steps ...
    steps: int = 0
    tool_errors: int = 0        # 观测为失败的步数
    asked: int = 0              # 主动向人提问的次数
    denied: int = 0             # 被权限策略拦下的次数
    has_report: bool = False    # 是否真的产出了诊断报告

    @property
    def stalled(self) -> bool:
        return self.status == "stalled"


def run_agent_on_sample(sample: Sample, build_loop: Callable[[Sample], Any],
                        tool_name: str = "diagnose_crash",
                        task_tpl: str = "诊断这次崩溃，ref={ref}") -> tuple[Report, AgentRunStats]:
    """
    让 agent 跑一个样本，取出诊断报告。

    返回 (Report, AgentRunStats)。报告取不到时返回一个 blocked 的空报告 ——
    **不能返回 None**：那样 `evaluate()` 会在 `.top()` 上炸，而炸掉会让
    整个 eval 停在半路，比一个明确的"未产出报告"难查得多。
    """
    loop = build_loop(sample)
    out: RunOutcome = loop.run(task_tpl.format(ref=sample.ref))

    stats = AgentRunStats(sample_id=sample.id, status=out.status,
                          steps=out.steps)

    report: Report | None = None
    for t in out.turns:
        if isinstance(t.step, Action) and t.step.tool == tool_name:
            if t.observation is not None and t.observation.data:
                r = t.observation.data.get("report")
                if isinstance(r, Report):
                    report = r
                    break
        if t.observation is not None and not t.observation.ok:
            stats.tool_errors += 1
        if type(t.step).__name__ == "AskUser":
            stats.asked += 1
        if t.observation is not None and getattr(t.observation, "status", "") == "denied":
            stats.denied += 1

    stats.has_report = report is not None
    if report is None:
        rep = Report()
        rep.status = "blocked"
        rep.block_reason = f"agent 未产出诊断报告（{out.status}）"
        return rep, stats
    return report, stats


# ---------------------------------------------------------------- 汇总

@dataclass
class AgentEvalReport:
    """agent 级报告 = 既有结论指标（可对比）+ 过程指标（agent 特有）。"""

    base: EvalReport
    runs: list[AgentRunStats] = field(default_factory=list)

    @property
    def avg_steps(self) -> float:
        return sum(r.steps for r in self.runs) / len(self.runs) if self.runs else 0.0

    @property
    def tool_error_rate(self) -> float:
        """工具调用失败率。高说明工具层或模型输出有问题，而不是能力不行。"""
        if not self.runs:
            return 0.0
        return sum(r.tool_errors for r in self.runs) / len(self.runs)

    @property
    def stall_rate(self) -> float:
        """停滞率。这是 agent 特有的失败模式：一直在动但没有进展。"""
        return (sum(1 for r in self.runs if r.stalled) / len(self.runs)
                if self.runs else 0.0)

    @property
    def no_report_rate(self) -> float:
        """压根没走到诊断工具的比例。高说明路由/派发有问题。"""
        return (sum(1 for r in self.runs if not r.has_report) / len(self.runs)
                if self.runs else 0.0)

    def render(self) -> str:
        lines = [self.base.render(), "", "agent 过程指标:"]
        lines += [
            f"  平均步数        {self.avg_steps:.2f}",
            f"  工具错误率      {self.tool_error_rate:.2%}",
            f"  停滞率          {self.stall_rate:.2%}",
            f"  未产出报告率    {self.no_report_rate:.2%}",
            f"  提问次数        {sum(r.asked for r in self.runs)}",
            f"  被拦下次数      {sum(r.denied for r in self.runs)}",
        ]
        return "\n".join(lines)


def evaluate_agent(samples: SampleSet,
                   build_loop: Callable[[Sample], Any],
                   repeat: int = 1,
                   tool_name: str = "diagnose_crash",
                   task_tpl: str = "诊断这次崩溃，ref={ref}") -> AgentEvalReport:
    """
    跑一轮 agent eval。

    build_loop(sample) -> AgentLoop，由调用方注入。
    这样同一份样本可以分别用「流水线」和「agent」跑，指标口径完全一致。

    注意 repeat>1 时 `evaluate()` 会额外调用 run_fn，过程指标也会把那些
    重复运行计入 —— 这是刻意的：稳定性统计本来就该包含它们。
    """
    runs: list[AgentRunStats] = []

    def run_fn(s: Sample) -> Report:
        rep, st = run_agent_on_sample(s, build_loop, tool_name, task_tpl)
        runs.append(st)
        return rep

    base = evaluate(samples, run_fn, repeat=repeat)
    return AgentEvalReport(base=base, runs=runs)
