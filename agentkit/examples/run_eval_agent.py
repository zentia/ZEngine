"""
agent 级 eval：**同一份样本、两条路径、同一把尺子**。

    cd agentkit
    python -m examples.run_eval_agent

一次跑出两组数字：

  流水线    diagnose() 直接调用（既有基线）
  agent     AgentLoop + diagnose_crash 工具

两组用**完全相同**的 `score()` 评分，所以差异只可能来自路径本身，
不可能来自指标口径 —— 这正是"能不能直接比"的全部意义。

预期结果：两条路径的数字应该**完全一致**（或极其接近）。
因为这里用的是确定性决策者（`ScriptedPlanner` / `FakeTransport`），
agent 只是多绕了一层调用。

**这个"一致"本身就是一个重要结论**：它证明 agent 层没有引入损耗，
之后接上真 LLM 时掉下来的每一个点，就都可以归因到模型，而不是管道。
如果没有这个对照实验，你看到的"掉了 13 个点"根本无法归因 ——
它可能是模型不行，也可能是你的工具层有 bug。
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from zagent.adapters import GitCodeIndex, LocalDumpSource, SimpleVersionResolver
from zagent.eval import Sample, SampleSet, evaluate
from zagent.eval_agent import evaluate_agent
from zagent.llm import FakeTransport, LLMPlanner, LLMResponse
from zagent.loop import (Action, AgentLoop, AutoInteractor, Finish,
                         ScriptedPlanner)
from zagent.pipeline import diagnose
from zagent.policy import Mode, Policy
from zagent.protocols import Context
from zagent.tools import ToolRegistry, make_shell_tool
from zagent.tools.crash import make_crash_tools
from zagent.tracing import Tracer
from zagent.verify import HeuristicReasoner, RuleVerifier

from run_eval import build_repo                       # noqa: E402
from minimal_run import build_crash_sample            # noqa: E402


def _call(name: str, args: dict) -> LLMResponse:
    """构造一条 function calling 响应（模拟模型输出）。"""
    return LLMResponse(tool_calls=[
        {"id": "c1", "type": "function",
         "function": {"name": name,
                      "arguments": json.dumps(args, ensure_ascii=False)}}])


def _build_samples(tmp: Path, repo: Path, head: str) -> tuple[SampleSet, dict[str, Path]]:
    """与 run_eval.py **完全相同**的样本集，保证可比。"""
    samples = SampleSet(name="demo-v1", samples=[
        Sample(id="p1", ref="", expected_path="src/actor.cpp", expected_line=22,
               note="注入 bug：WriteNameUnsafe 无空判断"),
        Sample(id="p2", ref="", expected_path="src/actor.cpp", expected_line=22,
               note="同上，栈帧更少"),
        Sample(id="n1", ref="", negative=True, note="版本错配，应被阻塞"),
        Sample(id="n2", ref="", negative=True, note="空栈，应声明无法定位"),
    ])

    files: dict[str, Path] = {}
    for s in samples.samples:
        p = tmp / f"{s.id}.json"
        if s.id == "n1":
            build_crash_sample(p, product_version="9.9.9.111111", line=22, frames=4)
        elif s.id == "n2":
            p.write_text(json.dumps({
                "platform": "Windows", "product_version": head,
                "exception_type": "EXCEPTION_ACCESS_VIOLATION", "stack": []},
            ), encoding="utf-8")
        else:
            build_crash_sample(p, product_version=head, line=22,
                               frames=4 if s.id == "p1" else 2)
        files[s.id] = p
        s.ref = str(p)          # ref 就是 dump 文件路径
    return samples, files


def main() -> int:
    use_llm = "--scripted" not in sys.argv

    tmp = Path(tempfile.mkdtemp(prefix="zagent_eval_agent_"))
    repo = tmp / "repo"
    trace_dir = tmp / "traces"

    print("=" * 68)
    print("ZEngine Agent Kit — agent 级 eval（与流水线基线对照）")
    print("=" * 68)

    head = build_repo(repo)
    samples, files = _build_samples(tmp, repo, head)

    code = GitCodeIndex(repo, project_globs=("src/",))
    reasoner = HeuristicReasoner(code)
    verifier = RuleVerifier()

    def ctx_for(dump: Path) -> Context:
        return Context(source=LocalDumpSource(dump),
                       version=SimpleVersionResolver(),
                       code=code, reasoner=reasoner, verifier=verifier)

    # ---------- 路径 A：流水线（基线） ----------
    def pipeline_run(s: Sample):
        return diagnose(s.ref, ctx_for(files[s.id]),
                        Tracer(f"pipe-{s.id}", trace_dir))

    base = evaluate(samples, pipeline_run, repeat=1)

    # ---------- 路径 B：agent ----------
    def resolve_ctx(ref: str) -> Context | None:
        p = Path(ref)
        return ctx_for(p) if p.exists() else None

    def build_loop(s: Sample) -> AgentLoop:
        reg = ToolRegistry()
        reg.register(make_shell_tool(cwd=str(repo), timeout=30))
        for t in make_crash_tools(resolve_ctx, trace_dir=trace_dir):
            reg.register(t)

        if use_llm:
            # 模拟"模型决定调用诊断工具"这一步。
            # 确定性，可复现 —— 这是本次对照实验的前提。
            planner = LLMPlanner(reg, transport=FakeTransport([
                _call("diagnose_crash", {"ref": s.ref}),
                _call("finish", {"answer": "已交由诊断工具", "unknowns": []}),
            ]))
        else:
            planner = ScriptedPlanner([
                Action("diagnose_crash", {"ref": s.ref}),
                Finish(answer="已交由诊断工具"),
            ])

        return AgentLoop(reg, planner, policy=Policy(mode=Mode.NORMAL),
                         interactor=AutoInteractor(approve_default=False),
                         max_steps=6)

    agent = evaluate_agent(samples, build_loop, repeat=1)

    # ---------- 对照 ----------
    print("\n[流水线基线]")
    print(base.render())

    print("\n[agent]")
    print(agent.render())

    print("\n[对照]")
    def _delta(name: str, a: float, b: float) -> str:
        d = b - a
        return f"  {name:<12} {a:>7.2%} -> {b:>7.2%}   {d:+.2%}"

    print(_delta("top1", base.top1, agent.base.top1))
    print(_delta("top3", base.top3, agent.base.top3))
    print(_delta("honesty", base.honesty, agent.base.honesty))
    print(_delta("evidence", base.evidence_completeness,
                 agent.base.evidence_completeness))

    print(f"\n注: 决策者 = {'模拟 LLM (FakeTransport)' if use_llm else 'ScriptedPlanner'}，"
          f"确定性，两条路径应一致。")
    print("    一致说明 agent 层无损耗；接真 LLM 后掉的点才可能归因到模型。")
    print(f"\n样本 {len(samples)} 个，无统计意义，用于验证通路与可比性。")
    print(f"轨迹: {trace_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
