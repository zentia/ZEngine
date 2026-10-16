"""
包入口。
"""

from .llm import (FakeTransport, LLMConfig, LLMError, LLMPlanner, LLMResponse,
                  LLMTransport, OpenAICompatTransport, SYSTEM_PROMPT)
from .eval_agent import (AgentEvalReport, AgentRunStats, evaluate_agent,
                         run_agent_on_sample)
from .loop import (Action, AgentLoop, AskUser, AutoInteractor, BadStep,
                   ConsoleInteractor, Finish, Interactor, Planner, RunOutcome,
                   ScriptedPlanner, Turn)
from .tools.crash import make_crash_tools
from .tools.verify_tool import RULE_PARAMS, make_verify_tools
from .pipeline import diagnose, render
from .policy import Decision, Mode, Policy
from .tools import (Risk, Tool, ToolRegistry, ToolResult, classify_command_risk,
                    make_code_tools, make_shell_tool)
from .tracing import Tracer
from .types import (Evidence, Facts, Hypothesis, Report, Revision, SourceLocation,
                    StackFrame, Verdict)
from .verify import CallChainRule, HeuristicReasoner, NullCheckRule, RuleVerifier

# CLI 相关**懒加载**，不在这里 import。
#
# 原因很实际：`zagent/__init__` 一旦导入 `.cli`，再用
# `python -m zagent.cli` 启动时，Python 会警告
# "'zagent.cli' found in sys.modules after import of package 'zagent'" ——
# 每次启动命令行都先吐一条警告，很难看，也会让人以为系统有问题。
# 而且入口层（argparse / git 探测）不该被 `import zagent` 顺带拖起来。
_CLI_EXPORTS = ("Session", "RulePlanner", "build_registry")


def __getattr__(name: str):
    if name in _CLI_EXPORTS:
        from . import cli
        return getattr(cli, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = [
    # 既有流水线
    "diagnose", "render", "Tracer",
    "Evidence", "Facts", "Hypothesis", "Report", "Revision",
    "SourceLocation", "StackFrame", "Verdict",
    "CallChainRule", "NullCheckRule", "RuleVerifier", "HeuristicReasoner",
    # agent 层
    "AgentLoop", "Planner", "Interactor", "ScriptedPlanner", "AutoInteractor",
    "ConsoleInteractor", "Action", "Finish", "AskUser", "BadStep", "Turn",
    "RunOutcome",
    # LLM 决策者
    "LLMPlanner", "LLMConfig", "LLMResponse", "LLMTransport",
    "OpenAICompatTransport", "FakeTransport", "LLMError", "SYSTEM_PROMPT",
    "Policy", "Mode", "Decision",
    "Tool", "ToolRegistry", "ToolResult", "Risk",
    "make_shell_tool", "make_code_tools", "make_crash_tools",
    "make_verify_tools", "RULE_PARAMS", "classify_command_risk",
    # 崩溃诊断降级为工具 + agent 级 eval
    "evaluate_agent", "run_agent_on_sample", "AgentEvalReport", "AgentRunStats",
    # CLI（入口）
    "Session", "RulePlanner", "build_registry",
]

__version__ = "0.3.0"
