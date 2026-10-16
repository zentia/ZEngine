"""
工具层。

把 zagent 已有的能力抽成「可被任意决策者枚举与调用」的工具表。

为什么先做这一层
----------------
现有的 `diagnose()` 是一条写死的五阶段流水线：决策者（目前是
`HeuristicReasoner`）只能接受流水线喂给它的东西，没有"选一个工具去查"
的能力。要让它变成 agent，第一步不是接 LLM —— 接了 LLM 而没有任何
可枚举的工具，它依然只能复述流水线给它的结论。

这一层与 zagent 现有协议的关系：
- `CodeIndex` / `SymbolResolver` 等 Protocol **原样复用**，只是外面包一层
  工具壳（`tools/code.py`），不改动验证规则的任何行为；
- `Verifier` 后续会作为一个**工具**暴露，而不是流水线的一个阶段 ——
  那时"何时该验证"由模型决定，"怎样算验证过"仍由规则把持。

崩溃诊断在这个体系里的位置
--------------------------
`tools/crash.py` 把 `diagnose()` **降级成一个工具**。方向很重要：不是把
流水线改写成 agent，而是让 agent 去调用它 —— 流水线因此保住确定性
（可回归、可 eval），agent 因此拿到自主性（决定何时调用、调用后干什么）。
崩溃诊断是这套 agent 的一个能力，不是它的全部。
"""

from .base import Risk, Tool, ToolRegistry, ToolResult
from .code import make_code_tools
from .crash import make_crash_tools
from .shell import classify_command_risk, make_shell_tool
from .verify_tool import RULE_PARAMS, make_verify_tools

__all__ = [
    "Risk", "Tool", "ToolRegistry", "ToolResult",
    "make_shell_tool", "make_code_tools", "make_crash_tools",
    "make_verify_tools", "RULE_PARAMS",
    "classify_command_risk",
]
