"""
权限策略：决定「这个动作要不要先问人」。

为什么不能沿用 auto_checkout 那种全局开关
-----------------------------------------
现有流水线用 `auto_checkout=False` 处理不确定：版本不一致就硬阻塞。
做诊断工具这是对的（切错分支的代价远大于停下来），但做 agent 这就是
"趋于 0 的自主性" —— 它会连安装一个依赖都不敢。

问题不在于"该不该自动"，而在于**用一个布尔值同时回答了三个不同风险等级
的问题**。正确的做法是分级：按动作的**可挽回性**裁决，而不是按开关。

分级依据（与 Risk 一致）：错了之后能不能挽回。

另一个关键：**低置信度本身也要触发询问**。这把「不确定性」从报告里的
一个标注，变成了控制流的一部分 —— 模型说"我不确定"，系统就真的去问人，
而不是照着不确定的猜测往下执行。
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from .tools.base import Risk


class Mode(str, Enum):
    """会话的自治档位。"""

    PLAN = "plan"            # 只做只读与检索，其余一律先问（适合让人审方案）
    NORMAL = "normal"        # 本地可撤销动作自动，破坏/出网/不可逆先问
    AUTOPILOT = "autopilot"  # 除不可逆外全部自动（无人值守批量任务）


@dataclass
class Decision:
    allow: bool
    need_ask: bool = False
    reason: str = ""

    @property
    def auto(self) -> bool:
        return self.allow and not self.need_ask


# 各档位的自动放行集合。写成集合而不是 if 链，是为了让"新增一个 Risk 时
# 必须显式决定它在每个档位的行为"—— 漏了就会落进 need_ask，即保守方向。
_AUTO: dict[Mode, frozenset[Risk]] = {
    Mode.PLAN: frozenset({Risk.READ, Risk.SEARCH}),
    Mode.NORMAL: frozenset({Risk.READ, Risk.SEARCH, Risk.WRITE, Risk.EXEC}),
    Mode.AUTOPILOT: frozenset({Risk.READ, Risk.SEARCH, Risk.WRITE, Risk.EXEC,
                               Risk.EXEC_DESTRUCTIVE, Risk.NETWORK}),
}

# 任何档位都不自动放行：错了无法挽回。
_NEVER_AUTO: frozenset[Risk] = frozenset({Risk.IRREVERSIBLE})


class Policy:
    """风险 -> 裁决。"""

    def __init__(self, mode: Mode = Mode.NORMAL,
                 min_confidence: float = 0.4) -> None:
        self.mode = mode
        # 低于该置信度的动作，若风险 >= EXEC 就转为询问。
        # 0.4 是刻意偏保守：让"不确定"真的能拦住动作，而不是只写进报告。
        self.min_confidence = min_confidence

    def decide(self, risk: Risk, confidence: float = 1.0) -> Decision:
        if risk in _NEVER_AUTO:
            return Decision(allow=False, need_ask=True,
                            reason=f"{risk.value} 不可撤销，任何档位都需人工确认")

        auto = risk in _AUTO.get(self.mode, frozenset())
        if not auto:
            return Decision(allow=False, need_ask=True,
                            reason=f"{risk.value} 在 {self.mode.value} 档位下需确认")

        # 置信度过低：即使动作本身被允许，也要先问。
        # 这是"不确定性驱动控制流"的落点 —— 不许模型拿着没把握的猜测继续跑。
        if confidence < self.min_confidence and risk in (
                Risk.EXEC, Risk.EXEC_DESTRUCTIVE, Risk.NETWORK, Risk.WRITE):
            return Decision(
                allow=False, need_ask=True,
                reason=(f"置信度 {confidence:.2f} 低于阈值 "
                        f"{self.min_confidence:.2f}，动作风险 {risk.value}"))

        return Decision(allow=True, need_ask=False)
