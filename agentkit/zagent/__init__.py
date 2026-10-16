"""
包入口。
"""

from .loop import (Action, AgentLoop, AskUser, AutoInteractor, ConsoleInteractor,
                   Finish, Interactor, Planner, RunOutcome, ScriptedPlanner, Turn)
from .pipeline import diagnose, render
from .policy import Decision, Mode, Policy
from .tools import (Risk, Tool, ToolRegistry, ToolResult, classify_command_risk,
                    make_code_tools, make_shell_tool)
from .tracing import Tracer
from .types import (Evidence, Facts, Hypothesis, Report, Revision, SourceLocation,
                    StackFrame, Verdict)
from .verify import CallChainRule, HeuristicReasoner, NullCheckRule, RuleVerifier

__all__ = [
    # 既有流水线
    "diagnose", "render", "Tracer",
    "Evidence", "Facts", "Hypothesis", "Report", "Revision",
    "SourceLocation", "StackFrame", "Verdict",
    "CallChainRule", "NullCheckRule", "RuleVerifier", "HeuristicReasoner",
    # agent 层
    "AgentLoop", "Planner", "Interactor", "ScriptedPlanner", "AutoInteractor",
    "ConsoleInteractor", "Action", "Finish", "AskUser", "Turn", "RunOutcome",
    "Policy", "Mode", "Decision",
    "Tool", "ToolRegistry", "ToolResult", "Risk",
    "make_shell_tool", "make_code_tools", "classify_command_risk",
]

__version__ = "0.2.0"
