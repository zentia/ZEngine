"""
Agent loop：模型在环的「思考 -> 行动 -> 观测」循环。

它与现有 `diagnose()` 的关系
----------------------------
`diagnose()` 是一条写死的五阶段流水线：**人替模型把推理顺序定死了**。
这个 loop 不做任何领域推理 —— 它只负责"让某个决策者反复看观测、决定
下一步"。领域知识全部留在工具里（尤其是 `Verifier`），不进 loop。

这样分工的理由：流水线适合"步骤确定、要可回归"的场景（你的 eval 依赖
这种确定性）；loop 适合"步骤不确定"的场景。两者不是替代关系 ——
`diagnose()` 应当整体降级成这个 loop 可以调用的一个工具。

三个防呆，每一个都对应一种真实的死法
------------------------------------
1. `max_steps` —— 没有它，一个拿不到满意结果的模型会无限循环烧 token。
   这是 ReAct 类系统第一号故障模式。
2. 停滞检测 —— 连续失败、或原样重复上一步动作，就停下来报 `stalled`。
   "一直在动但毫无进展"比"卡住不动"更难被发现，也更贵。
3. 工具调用永不抛异常 —— 由 `ToolRegistry.call` 保证，错误降级为观测，
   模型因此有机会自我纠正。

交互：不是"阻塞等输入"，而是决策者的一种输出
--------------------------------------------
`AskUser` 与 `Action`、`Finish` 并列，是决策者可以主动选择的一种 step。
区别在于：阻塞是系统替模型决定"到此为止"，而这里是模型自己判断
"我需要人"。前者做不出 agent，后者才做得出。
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Callable, Protocol

if TYPE_CHECKING:  # 仅类型标注：运行时不能 import gate，否则与 gate 成环
    from .gate import ConclusionGate

from .gate import STATUS_GATE_REJECTED
from .policy import Policy
from .tools.base import STATUS_DENIED, RISK_ORDER, Risk, ToolRegistry, ToolResult
from .tools.shell import classify_command_risk

STATUS_DONE = "done"
STATUS_BLOCKED = "blocked"
STATUS_MAX_STEPS = "max_steps"
STATUS_STALLED = "stalled"
STATUS_ERROR = "error"


# ---------------------------------------------------------------- step 类型

@dataclass
class Action:
    """调用一个工具。"""

    tool: str
    args: dict[str, Any] = field(default_factory=dict)
    thought: str = ""          # 为什么做这一步，进轨迹供复盘
    confidence: float = 1.0    # 模型对"这一步是对的"的把握


@dataclass
class Finish:
    """
    结束。answer 给结论，unknowns 说明没查清的部分。

    conclusions 是**显式声明的结论**，与 answer 里的叙述分开填。
    门禁只裁定这里列出的条目 —— 不从 answer 的自然语言里猜。
    声明是模型自己给的，所以判定是确定的；从一段话里判断"它是不是在
    断言"做不准，做不准的门禁会误杀正确的收尾。
    """

    answer: str
    unknowns: list[str] = field(default_factory=list)
    conclusions: list[str] = field(default_factory=list)
    confidence: float = 1.0


@dataclass
class AskUser:
    """主动向人提问。options 为空表示自由回答。"""

    question: str
    options: list[str] = field(default_factory=list)
    context: str = ""


@dataclass
class BadStep:
    """
    决策者产出了无法执行的步骤：JSON 解析失败、调用了不存在的工具、
    LLM 请求本身失败等。

    **为什么它需要是一个独立的 step 类型，而不是让 Planner 抛异常**：
    畸形输出是 LLM 最高频的故障，而且绝大多数是可恢复的 —— 模型只要
    看到"你上一步的 JSON 括号没闭合"就能自己改对。把它变成异常，等于
    把最常见的可恢复错误升级成致命错误；把它变成观测，它就成了模型可以
    自我纠正的一条反馈。
    """

    reason: str
    raw: str = ""


Step = Action | Finish | AskUser | BadStep


@dataclass
class Turn:
    """一轮。三种 step 共用，便于统一进轨迹。"""

    index: int
    step: Step
    observation: ToolResult | None = None
    answer: str | None = None       # AskUser 的回答
    approved: bool = True           # 权限裁决结果

    @property
    def failed(self) -> bool:
        return self.observation is not None and not self.observation.ok


class Planner(Protocol):
    """决策者。LLM 实现它；规则实现它；测试用脚本实现它。"""

    def plan(self, task: str, history: list[Turn],
             pending: list[str]) -> Step:
        ...


class Interactor(Protocol):
    """人。抽成接口是为了让无人值守场景可替换，也为了可测试。"""

    def ask(self, req: AskUser) -> str:
        ...

    def approve(self, question: str) -> bool:
        ...


class AutoInteractor:
    """
    自动应答。用于测试与 CI。

    `approve_default=False` 是默认值，理由是：一个默认同意一切的实现，
    会让"权限策略真的生效了吗"这个问题在测试里永远测不出来。要放开，
    调用方必须显式写出来。
    """

    def __init__(self, approve_default: bool = False, answer: str = "") -> None:
        self.approve_default = approve_default
        self.answer = answer

    def ask(self, req: AskUser) -> str:
        return self.answer or (req.options[0] if req.options else "")

    def approve(self, question: str) -> bool:
        return self.approve_default


class ConsoleInteractor:
    """命令行真人应答。"""

    def ask(self, req: AskUser) -> str:
        print(f"\n[提问] {req.question}")
        if req.options:
            for i, opt in enumerate(req.options, 1):
                print(f"  {i}. {opt}")
        return input("你的回答: ").strip()

    def approve(self, question: str) -> bool:
        print(f"\n[需要确认] {question}")
        ans = input("允许? (y/N): ").strip().lower()
        return ans in ("y", "yes", "是")


# ---------------------------------------------------------------- 结果

@dataclass
class RunOutcome:
    status: str = STATUS_ERROR
    answer: str = ""
    unknowns: list[str] = field(default_factory=list)
    conclusions: list[str] = field(default_factory=list)
    turns: list[Turn] = field(default_factory=list)

    @property
    def steps(self) -> int:
        return len(self.turns)

    @property
    def ok(self) -> bool:
        return self.status == STATUS_DONE


# ---------------------------------------------------------------- loop

class AgentLoop:
    def __init__(self, registry: ToolRegistry, planner: Planner,
                 policy: Policy | None = None,
                 interactor: Interactor | None = None,
                 max_steps: int = 20,
                 max_consecutive_failures: int = 3,
                 on_turn: Callable[[Turn], None] | None = None,
                 gate: "ConclusionGate | None" = None) -> None:
        self.registry = registry
        self.planner = planner
        self.policy = policy or Policy()
        self.interactor = interactor or AutoInteractor()
        self.max_steps = max_steps
        self.max_consecutive_failures = max_consecutive_failures
        # 结论门禁。None 表示不启用 —— 默认关闭是为了让既有 eval
        # 的对照实验保持干净（改一次数字就无法归因了），
        # CLI 与诊断类任务显式装上。
        self.gate = gate
        # 每轮完成即回调。CLI 靠它实时打印（否则用户盯着黑屏等几十秒），
        # eval 靠它逐步记账。抽成回调而不是在 loop 里直接 print，是因为
        # loop 不该知道自己在被谁驱动。
        self.on_turn = on_turn
        self._inbox: list[str] = []
        self._lock = threading.Lock()

    def _record(self, history: list[Turn], turn: Turn) -> None:
        history.append(turn)
        if self.on_turn is not None:
            try:
                self.on_turn(turn)
            except Exception:  # noqa: BLE001 —— 回调出错不许掀翻 loop
                pass

    def push(self, message: str) -> None:
        """
        注入一条用户消息（steering）。

        这是"可中断"的落点：用户不必等这一轮跑完，消息会排进队列，
        在下一轮决策前交给决策者。没有这条通道，交互就只能发生在
        任务开始和结束两个点上。
        """
        with self._lock:
            self._inbox.append(message)

    def _drain(self) -> list[str]:
        with self._lock:
            out = list(self._inbox)
            self._inbox.clear()
        return out

    def run(self, task: str) -> RunOutcome:
        history: list[Turn] = []
        consecutive_failures = 0

        for i in range(self.max_steps):
            pending = self._drain()
            step = self.planner.plan(task, history, pending)

            if isinstance(step, Finish):
                # ---- 结论硬门禁 ----
                # 护栏放在 loop 而不是 planner：planner 可以被替换，
                # 而这道约束必须对**所有**决策者都成立。放错地方的话，
                # 换一个 planner 就等于把护栏拆了。
                if self.gate is not None:
                    verdict = self.gate.check(step, history)
                    if not verdict.allowed:
                        # 被拒的收尾**不终结运行**：变成一条观测塞回轨迹，
                        # 模型下一步会读到"为什么被拒"，有机会自己改。
                        # 直接抛异常或强行收尾都更糟 —— 前者丢失结论，
                        # 后者等于门禁没生效。
                        self._record(history, Turn(
                            index=i, step=step,
                            observation=ToolResult(
                                ok=False, status=STATUS_GATE_REJECTED,
                                error=verdict.reason)))
                        consecutive_failures += 1
                        if consecutive_failures >= self.max_consecutive_failures:
                            return RunOutcome(
                                status=STATUS_STALLED, turns=list(history),
                                answer=f"结论未通过门禁，连续 "
                                       f"{consecutive_failures} 次被拒："
                                       f"{verdict.reason}",
                                unknowns=list(step.unknowns))
                        continue

                # 结束本身也进轨迹。不记这一条，复现时就看不到"决策者是在
                # 什么观测之后决定收尾的"，也就无从判断它是不是收早了。
                self._record(history, Turn(index=i, step=step))
                return RunOutcome(status=STATUS_DONE, answer=step.answer,
                                  unknowns=list(step.unknowns),
                                  conclusions=list(step.conclusions),
                                  turns=list(history))

            if isinstance(step, AskUser):
                answer = self.interactor.ask(step)
                self._record(history, Turn(index=i, step=step, answer=answer))
                continue

            if isinstance(step, BadStep):
                # 不终结运行：把它变成一条模型能读懂的失败观测，
                # 让它有机会在下一步自己纠正。
                self._record(history, Turn(
                    index=i, step=step,
                    observation=ToolResult(ok=False, status=STATUS_ERROR,
                                           error=step.reason)))
                consecutive_failures += 1
                if consecutive_failures >= self.max_consecutive_failures:
                    return RunOutcome(
                        status=STATUS_STALLED, turns=list(history),
                        answer=f"连续 {consecutive_failures} 步无法执行，"
                               f"最后原因：{step.reason}")
                continue

            # ---- 权限裁决 ----
            risk = self._risk_of(step)
            decision = self.policy.decide(risk, step.confidence)

            if decision.need_ask:
                ok = self.interactor.approve(
                    f"即将执行 {step.tool}({_brief(step.args)}) —— {decision.reason}")
                if not ok:
                    self._record(history, Turn(
                        index=i, step=step, approved=False,
                        observation=ToolResult(ok=False, status=STATUS_DENIED,
                                               error=f"未获批准：{decision.reason}")))
                    consecutive_failures += 1
                    if consecutive_failures >= self.max_consecutive_failures:
                        return RunOutcome(status=STATUS_STALLED,
                                          turns=list(history),
                                          answer="连续未获批准，停止")
                    continue

            # ---- 执行 ----
            obs = self.registry.call(step.tool, step.args)
            self._record(history, Turn(index=i, step=step, observation=obs))

            # ---- 停滞检测 ----
            if obs.ok:
                consecutive_failures = 0
            else:
                consecutive_failures += 1
                if consecutive_failures >= self.max_consecutive_failures:
                    return RunOutcome(
                        status=STATUS_STALLED, turns=list(history),
                        answer=(f"连续 {consecutive_failures} 步失败，"
                                f"最后一步 {step.tool} 返回 [{obs.status}]"))

            if _is_repeat(history):
                return RunOutcome(
                    status=STATUS_STALLED, turns=list(history),
                    answer=f"重复执行了与上一步相同的动作 {step.tool}，停止")

        return RunOutcome(status=STATUS_MAX_STEPS, turns=list(history),
                          answer=f"达到步数上限 {self.max_steps}")

    def _risk_of(self, step: Action) -> Risk:
        """
        取动作的实际风险。

        shell 工具要**看命令内容**再定级：工具表里写的是 EXEC，但
        `rm -rf` 和 `ls` 显然不是一回事。只按工具名定级，等于让所有
        shell 调用都以同一个低等级通过 —— 权限分级就形同虚设。
        """
        base = self.registry.risk_of(step.tool)
        if step.tool == "shell":
            cmd = str(step.args.get("command", ""))
            actual = classify_command_risk(cmd)
            return actual if RISK_ORDER[actual] > RISK_ORDER[base] else base
        return base


def _brief(args: dict[str, Any], limit: int = 80) -> str:
    text = ", ".join(f"{k}={v}" for k, v in args.items())
    return text[:limit] + ("..." if len(text) > limit else "")


def _is_repeat(history: list[Turn]) -> bool:
    """最后两步是否原样重复。重复即无进展，继续跑只是在烧 token。"""
    if len(history) < 2:
        return False
    a, b = history[-2].step, history[-1].step
    if not (isinstance(a, Action) and isinstance(b, Action)):
        return False
    return a.tool == b.tool and a.args == b.args


class ScriptedPlanner:
    """
    按脚本依次给出 step。用于测试 loop 本身，不依赖任何模型。

    **为什么必须先有它**：接 LLM 之前如果没法把 loop 跑通，那么之后
    出现的每一个问题都无法归因 —— 不知道是 loop 的错还是模型的错。
    这与 `HeuristicReasoner` 在流水线里的角色完全相同：先有一个确定性
    基线，再谈替换。
    """

    def __init__(self, steps: list[Step], seen_pending: list[list[str]] | None = None) -> None:
        self._steps = list(steps)
        self.seen_pending = seen_pending if seen_pending is not None else []

    def plan(self, task: str, history: list[Turn],
             pending: list[str]) -> Step:
        self.seen_pending.append(list(pending))
        if not self._steps:
            return Finish(answer="脚本耗尽")
        return self._steps.pop(0)
