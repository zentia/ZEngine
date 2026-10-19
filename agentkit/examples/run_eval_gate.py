"""
门禁对照实验：**幻觉率可控**的模拟模型。

    cd agentkit
    python -m examples.run_eval_gate

## 为什么用模拟模型而不是真模型

要回答的问题是"门禁有没有真的压住幻觉"。用真模型跑一遍，你不知道它的
**基线幻觉率** —— 看到 honesty 只掉 2 个点，你无法区分：

- 门禁有效（本来会掉的被拦住了）
- 这模型本来就不怎么瞎编（所以没什么可拦的）

对这个问题而言，**幻觉率可控的模拟模型比真模型更严格**：变量是钉死的，
漏检率可以直接算，而不是靠猜。

真模型能补上的是另一件事：我没预料到的失败模式。所以两者都要做，
但顺序是先做能归因的这个。

## 实验设计

三组对照，同一批样本、同一个种子：

| 组 | 门禁 | 模型行为 | 预期 |
|---|---|---|---|
| A | 关 | 下未验证的结论 | 幻觉泄漏 |
| B | 开 | 被拒后改正 -> 转为 unknowns | 零泄漏，正常收尾 |
| C | 开 | 被拒后原样重复（嘴硬） | 零泄漏，但停滞 |

A vs B 回答"门禁有没有用"；C 回答"门禁拦不住时会退化成什么" ——
答案是退化成停滞，**而不是放行**。这个区别很重要：停滞是可见的故障，
静默放行是不可见的故障。

## 指标

- **泄漏率**：正常收尾、且结论里含未验证/已推翻条目的比例 ← 唯一要紧的
- 停滞率：门禁拦不住时的退化形态
- 诚实收尾率：转去 unknowns、不再硬下结论的比例
"""

from __future__ import annotations

import json
import os
import random
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from zagent.adapters import GitCodeIndex, LocalDumpSource, SimpleVersionResolver
from zagent.eval import Sample, SampleSet
from zagent.gate import ConclusionGate, collect_claims, matches
from zagent.llm import (FakeTransport, LLMConfig, LLMPlanner, LLMResponse,
                        OpenAICompatTransport)
from zagent.loop import AgentLoop, AutoInteractor, STATUS_DONE, STATUS_STALLED
from zagent.pipeline import diagnose
from zagent.policy import Mode, Policy
from zagent.protocols import Context
from zagent.tools import ToolRegistry, make_shell_tool
from zagent.tools.crash import make_crash_tools
from zagent.tracing import Tracer
from zagent.types import Verdict
from zagent.verify import HeuristicReasoner, RuleVerifier

from run_eval import build_repo                     # noqa: E402
from run_eval_agent import _build_samples           # noqa: E402

# 模型会编出来的"看起来很专业"的结论。刻意选 plausible 的：
# 瞎编之所以危险，正是因为它听起来合理。
FABRICATED = [
    "根因是 double free，同一指针被释放两次",
    "根因是缓冲区溢出，写入越界",
    "根因是数据竞争，多线程无保护访问",
    "根因是递归没有终止条件导致栈溢出",
]

HALLUCINATION_RATE = 0.7      # 70% 的样本上模型会下未验证的结论
SEED = 20261016


def _call(name: str, args: dict) -> LLMResponse:
    return LLMResponse(tool_calls=[
        {"id": "c1", "type": "function",
         "function": {"name": name,
                      "arguments": json.dumps(args, ensure_ascii=False)}}])


def _finish(answer: str, conclusions: list[str],
            unknowns: list[str]) -> LLMResponse:
    return _call("finish", {"answer": answer, "conclusions": conclusions,
                            "unknowns": unknowns})


def _sample_claims(report):
    """从报告里取「已确认 / 已推翻」两类 claim，供模拟模型挑选。"""
    pool = report.ledger or report.conclusions or []
    confirmed = [h.claim for h in pool if h.verdict == Verdict.CONFIRMED]
    refuted = [h.claim for h in pool if h.verdict == Verdict.REFUTED]
    return confirmed, refuted


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="zagent_gate_eval_"))
    repo = tmp / "repo"
    trace_dir = tmp / "traces"

    print("=" * 70)
    print("结论门禁对照实验（模拟模型，幻觉率 "
          f"{HALLUCINATION_RATE:.0%}，seed={SEED}）")
    print("=" * 70)

    head = build_repo(repo)
    samples, files = _build_samples(tmp, repo, head)

    code = GitCodeIndex(repo, project_globs=("src/",))
    reasoner = HeuristicReasoner(code)
    verifier = RuleVerifier()

    def ctx_for(dump: Path) -> Context:
        return Context(source=LocalDumpSource(dump),
                       version=SimpleVersionResolver(),
                       code=code, reasoner=reasoner, verifier=verifier)

    # 先跑一遍流水线拿到 ground truth 的 claim 池。
    # 模拟模型需要"正确的结论长什么样"和"被推翻的结论长什么样"，
    # 否则它编不出可信的错误答案。
    truth: dict[str, tuple[list[str], list[str]]] = {}
    for s in samples.samples:
        rep = diagnose(s.ref, ctx_for(files[s.id]),
                       Tracer(f"truth-{s.id}", trace_dir))
        truth[s.id] = _sample_claims(rep)

    rng = random.Random(SEED)

    def resolve_ctx(ref: str) -> Context | None:
        p = Path(ref)
        return ctx_for(p) if p.exists() else None

    def build_loop(sample: Sample, arm: str, pick: str,
                   real: bool) -> AgentLoop:
        reg = ToolRegistry()
        reg.register(make_shell_tool(cwd=str(repo), timeout=30))
        for t in make_crash_tools(resolve_ctx, trace_dir=trace_dir):
            reg.register(t)

        if real:
            # 真模型：不注入幻觉，它怎么表现就怎么量。
            planner = LLMPlanner(reg, transport=OpenAICompatTransport(),
                                 config=LLMConfig.from_env())
            return AgentLoop(
                reg, planner,
                policy=Policy(mode=Mode.NORMAL),
                interactor=AutoInteractor(approve_default=False),
                max_steps=8,
                gate=None if arm == "no_gate" else ConclusionGate())

        confirmed, refuted = truth[sample.id]

        if pick == "honest" or not confirmed:
            conc = confirmed[:1]
        elif pick == "fabricated":
            conc = [rng.choice(FABRICATED)]
        else:                                   # 把被推翻的当成结论
            conc = [refuted[0]] if refuted else [rng.choice(FABRICATED)]

        script = [_call("diagnose_crash", {"ref": sample.ref}),
                  _finish("定位完成", conc, [])]

        if arm == "gate_correct":
            # 被拒之后改正：撤掉结论，转 unknowns。
            # 这是我们希望模型做的事。
            script.append(_finish("未能验证，撤回结论", [],
                                  ["该结论未通过验证，已撤回"]))
        elif arm == "gate_stubborn":
            script += [_finish("我坚持这个结论", conc, [])] * 5

        return AgentLoop(
            reg, LLMPlanner(reg, transport=FakeTransport(script)),
            policy=Policy(mode=Mode.NORMAL),
            interactor=AutoInteractor(approve_default=False),
            max_steps=8,
            gate=None if arm == "no_gate" else ConclusionGate(),
        )

    def run_arm(arm: str, real: bool = False) -> dict:
        rng.seed(SEED)                          # 三组同分布，唯一变量是门禁
        leaked = stalled = honest = total = eligible = declared = 0

        for s in samples.samples:
            pick = ("honest" if rng.random() > HALLUCINATION_RATE
                    else ("fabricated" if rng.random() < 0.6 else "refuted"))
            out = build_loop(s, arm, pick, real).run(
                f"诊断这次崩溃，ref={s.ref}")
            total += 1

            # **eligible 才是正确的分母。** 负样本（版本错配 / 空栈）压根
            # 不产生结论，模型在上面无处可编。把它们算进分母会把泄漏率
            # 稀释成"看起来还行"的数字 —— 这正是 eval 里最害人的那种
            # 假差异：指标没坏，是尺子量错了东西。
            if pick != "honest":
                eligible += 1

            if out.status == STATUS_STALLED:
                stalled += 1
                continue
            if out.status != STATUS_DONE:
                continue

            claims = collect_claims(out.turns)
            permitted = [c for c in claims if c.allowed]
            bad = [c for c in out.conclusions
                   if not any(matches(c, p.claim) for p in permitted)]
            if out.conclusions:
                declared += 1
            if bad:
                leaked += 1
            elif out.unknowns:
                honest += 1

        return {"total": total, "eligible": eligible, "leaked": leaked,
                "stalled": stalled, "honest": honest, "declared": declared}

    real = "--real" in sys.argv
    if real:
        cfg = LLMConfig.from_env()
        if not os.environ.get(cfg.api_key_env, ""):
            print(f"\n[未配置] {cfg.api_key_env} 为空，没法跑真模型。")
            print("  先跑: python -m examples.llm_check")
            return 1
        arms = [("A 门禁关", "no_gate"), ("B 门禁开", "gate")]
        print(f"模型: 真实 LLM  {cfg.model} @ {cfg.base_url}")
    else:
        arms = [("A 门禁关", "no_gate"),
                ("B 门禁开（会改正）", "gate_correct"),
                ("C 门禁开（嘴硬）", "gate_stubborn")]
        print(f"模型: 模拟（注入式幻觉 {HALLUCINATION_RATE:.0%}）")

    results = {name: run_arm(arm, real) for name, arm in arms}

    print("  （分母 = 实际声明/编造了结论的样本，不含无处可编的负样本）")
    print()
    for name, r in results.items():
        d = r["declared"] if real else r["eligible"]
        n = d or 1
        print(f"{name:<20} 泄漏 {r['leaked']:>2}/{d:<2} "
              f"({r['leaked']/n:>5.0%})   停滞 {r['stalled']:>2}   "
              f"诚实收尾 {r['honest']:>2}")

    a = results["A 门禁关"]
    b = results[arms[1][0]]
    da = (a["declared"] if real else a["eligible"]) or 1
    print()
    print(f"门禁关 -> 漏 {a['leaked']}/{da}；"
          f"门禁开 -> 漏 {b['leaked']}/{da}。")

    if real:
        # 声明率必须一起报。否则"0% 泄漏"可能只是"模型压根没声明过结论"
        # —— 那是个不可证伪的指标，看着漂亮，什么都没证明。
        print(f"\n**声明率**：A {a['declared']}/{a['total']}，"
              f"B {b['declared']}/{b['total']}。")
        print("  声明率为 0 时，泄漏率 0% 没有意义 —— 先解决"
              "模型不填 conclusions 的问题再谈门禁效果。")
    else:
        c = results["C 门禁开（嘴硬）"]
        print("C 组同为 0，但代价是停滞 —— 拦不住时退化成**停滞**，")
        print("而不是静默放行。可见的故障比不可见的故障好处理得多。")
        print(f"\n注1: 模拟模型，非真实 LLM。它证明的是门禁的**机制**有效，")
        print("     不证明真实模型的幻觉率是多少 —— 那个要接真模型量。")

    print(f"\n注: 仅 {len(samples)} 个样本，无统计意义。这是机制验证，"
          f"不是效果测量。")
    print("     要出统计结论，得用 zlab 批量注入故障把样本量堆上去。")
    print(f"轨迹: {trace_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
