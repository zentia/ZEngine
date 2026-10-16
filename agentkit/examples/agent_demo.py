"""
agent loop 最小可跑演示。不需要 LLM，不需要编译引擎。

    cd agentkit
    python -m examples.agent_demo

它演示的是三个痛点各自被解决之后的样子：

1. 一条会挂住的命令，不再"一直等待" —— 它会在 1.5 秒内返回
   `awaiting_input`，并把原因说清楚；
2. 一条破坏性命令不会被执行，也不会让整个任务卡死 —— 它被策略拦下，
   变成一条模型可以看到并据此改道的观测；
3. 任务执行中途可以插话（steering），决策者下一轮就能读到。

为什么用 ScriptedPlanner 而不是真模型
-------------------------------------
和 `HeuristicReasoner` 在流水线里的角色一样：**先有确定性基线**。
接 LLM 之前如果 loop 本身跑不通，之后出的每个问题都无法归因 ——
不知道是 loop 的错还是模型的错。
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.loop import (Action, AgentLoop, AskUser, AutoInteractor, Finish,
                         ScriptedPlanner)
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry, make_shell_tool

PY = sys.executable


def main() -> int:
    reg = ToolRegistry()
    # stall_timeout=1.5：演示用。真实场景默认关闭，因为编译类任务
    # 长时间无输出是常态，开启会误杀。
    reg.register(make_shell_tool(timeout=30, stall_timeout=1.5))

    # 故意安排三条"会出事"的动作，看 loop 怎么处理它们。
    planner = ScriptedPlanner([
        Action(tool="shell",
               args={"command": f'"{PY}" -c "input()"',
                     "timeout": 5},
               thought="先看看这个脚本是不是交互式的"),

        Action(tool="shell",
               args={"command": "rm -rf build"},
               thought="清掉构建目录重编",
               confidence=0.9),

        Action(tool="shell",
               args={"command": f'"{PY}" -c "print(\'ok\')"'},
               thought="确认环境可用"),

        AskUser(question="要继续跑全量 eval 吗?", options=["跑", "先不用"]),

        Finish(answer="环境可用；rm 未获批准，已跳过",
               unknowns=["是否已清掉旧的构建产物"]),
    ])

    loop = AgentLoop(
        reg, planner,
        policy=Policy(mode=Mode.NORMAL),
        interactor=AutoInteractor(approve_default=False, answer="先不用"),
        max_steps=10,
    )

    # 执行中途插话：这是"可中断"的落点
    loop.push("注意：不要在没确认前删任何东西")

    out = loop.run("确认这台机器能不能跑实验台")

    print(f"\n状态: {out.status}   步数: {out.steps}")
    print(f"结论: {out.answer}")
    if out.unknowns:
        print(f"未知项: {out.unknowns}")
    print("\n轨迹:")
    for t in out.turns:
        kind = type(t.step).__name__
        if isinstance(t.step, Action):
            detail = f"{t.step.tool}({t.step.args.get('command', '')[:48]})"
            if t.step.thought:
                detail = f"{t.step.thought} -> {detail}"
        elif isinstance(t.step, AskUser):
            detail = f"问: {t.step.question} (答: {t.answer})"
        else:
            detail = t.step.answer
        obs = t.observation.to_observation().replace("\n", " | ")[:90] \
            if t.observation else ""
        print(f"  [{t.index}] {kind:<6} {detail}")
        if obs:
            print(f"        观测: {obs}")

    print("\n决策者收到的插话:", planner.seen_pending)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
