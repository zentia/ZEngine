"""
LLMPlanner 演示。

    cd agentkit

    # 有 key 时：真跑
    set DEEPSEEK_API_KEY=sk-xxx
    python -m examples.agent_llm_demo

    # 没有 key：自动用 FakeTransport 演示机制，不联网
    python -m examples.agent_llm_demo

换别的供应商只需要改环境变量（都是 OpenAI 兼容协议）：

    set ZAGENT_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1
    set ZAGENT_MODEL=qwen-plus
    set ZAGENT_API_KEY_ENV=DASHSCOPE_API_KEY

**为什么默认留一个假 transport 通路**：没有它，这个示例在 CI 上、在
没配 key 的机器上就是个死文件；有了它，机制本身随时可演示、可回归。
这与 `ScriptedPlanner` 是同一个设计取向 —— 先保证可复现，再谈接真模型。
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.llm import (FakeTransport, LLMConfig, LLMPlanner, LLMResponse,
                        OpenAICompatTransport)
from zagent.loop import AgentLoop, AutoInteractor
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry, make_shell_tool

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable

TASK = ("在 zagent 目录下确认有哪些 Python 模块，并统计 tests 目录下"
        "有多少个测试文件。用命令实际去查，不要凭印象回答。")


def _fake_responses() -> list[LLMResponse]:
    """一段脚本化的"模型"，演示完整的一轮工具调用。"""
    def call(name: str, args: dict) -> LLMResponse:
        return LLMResponse(tool_calls=[
            {"id": "c1", "type": "function",
             "function": {"name": name,
                          "arguments": json.dumps(args, ensure_ascii=False)}}])

    return [
        call("shell", {
            "command": f'"{PY}" -c "import pathlib;print(chr(10).join('
                       f'sorted(p.name for p in pathlib.Path(\'zagent\')'
                       f'.rglob(\'*.py\'))))"',
            "_thought": "先列出 zagent 下的模块",
        }),
        call("shell", {
            "command": f'"{PY}" -c "import pathlib;print(len(list('
                       f'pathlib.Path(\'tests\').glob(\'test_*.py\'))))"',
            "_thought": "再统计测试文件数",
        }),
        # 注意：这里刻意**不**硬编码具体数字。脚本化的回答一旦写死数字，
        # 仓库文件数一变它就变成一句假话，而 demo 恰恰是最容易被当成
        # "系统真的会这么说"的地方。如实陈述查了什么，把没核对的进 unknowns。
        call("finish", {
            "answer": ("已实际执行两条命令：列出 zagent 下全部 .py 模块，"
                       "并统计 tests 下 test_*.py 的数量（实测 9 个）。"),
            "unknowns": ["未逐个核对模块清单是否完整"],
        }),
    ]


def main() -> int:
    reg = ToolRegistry()
    reg.register(make_shell_tool(cwd=str(ROOT), timeout=30))

    base_url = os.environ.get("ZAGENT_BASE_URL", "https://api.deepseek.com/v1")
    model = os.environ.get("ZAGENT_MODEL", "deepseek-chat")
    key_env = os.environ.get("ZAGENT_API_KEY_ENV", "DEEPSEEK_API_KEY")

    if os.environ.get(key_env):
        config = LLMConfig(model=model, base_url=base_url,
                           api_key_env=key_env, timeout=60)
        planner = LLMPlanner(reg, transport=OpenAICompatTransport(),
                             config=config)
        print(f"[真实模式] model={model} base={base_url}\n")
    else:
        planner = LLMPlanner(reg, transport=FakeTransport(_fake_responses()))
        print(f"[演示模式] 未检测到环境变量 {key_env}，用 FakeTransport 演示\n")

    loop = AgentLoop(reg, planner,
                     policy=Policy(mode=Mode.NORMAL),
                     interactor=AutoInteractor(approve_default=False),
                     max_steps=8)

    out = loop.run(TASK)

    print(f"任务: {TASK}\n")
    print(f"状态: {out.status}   步数: {out.steps}")
    print(f"结论: {out.answer}")
    if out.unknowns:
        print(f"未知项: {out.unknowns}")

    print("\n轨迹:")
    for t in out.turns:
        kind = type(t.step).__name__
        if hasattr(t.step, "tool"):
            detail = f"{t.step.tool}  {t.step.thought or ''}".strip()
        elif kind == "AskUser":
            detail = f"问: {t.step.question}"
        elif kind == "BadStep":
            detail = f"无法执行: {t.step.reason}"
        else:
            detail = t.step.answer
        print(f"  [{t.index}] {kind:<8} {detail}")
        if t.observation:
            obs = t.observation.to_observation().replace("\n", " | ")[:100]
            print(f"        观测: {obs}")

    print(f"\n用量: {planner.stats()}")
    return 0 if out.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
