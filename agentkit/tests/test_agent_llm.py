"""
LLMPlanner 的回归测试。全部走 FakeTransport，不联网。

    cd agentkit
    python -m unittest tests.test_agent_llm -v

重点是**失败路径**：畸形 JSON、未知工具、模型服务报错，都必须变成
BadStep 而不是异常。这部分比"开心路径"重要得多 —— 真正在线上出问题的
全是这些，而它们恰恰是最容易被漏测的。
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.llm import (ASK_USER, FINISH, FakeTransport, LLMConfig, LLMError,
                        LLMPlanner, LLMResponse, SYSTEM_PROMPT,
                        _extract_json, _finish_from_text)
from zagent.loop import (STATUS_DONE, STATUS_STALLED, Action, AgentLoop,
                         AskUser, AutoInteractor, BadStep, Finish)
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry, make_shell_tool

PY = sys.executable


def _call(name: str, args: dict | str, call_id: str = "c1") -> LLMResponse:
    if not isinstance(args, str):
        args = json.dumps(args, ensure_ascii=False)
    return LLMResponse(tool_calls=[
        {"id": call_id, "type": "function",
         "function": {"name": name, "arguments": args}}
    ])


def _text(content: str) -> LLMResponse:
    return LLMResponse(content=content)


class PlannerTest(unittest.TestCase):
    def setUp(self):
        self.reg = ToolRegistry()
        self.reg.register(make_shell_tool(timeout=20))

    def _planner(self, responses) -> tuple[LLMPlanner, FakeTransport]:
        t = FakeTransport(responses)
        return LLMPlanner(self.reg, transport=t), t

    # ---- 开心路径 ----

    def test_tool_call_becomes_action(self):
        p, _ = self._planner([_call("shell", {"command": "echo hi"})])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, Action)
        self.assertEqual(step.tool, "shell")
        self.assertEqual(step.args["command"], "echo hi")

    def test_reserved_keys_stripped(self):
        """_thought / _confidence 不能流进工具实参。"""
        p, _ = self._planner([_call("shell", {
            "command": "echo hi", "_thought": "先探一下",
            "_confidence": 0.3})])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, Action)
        self.assertEqual(step.args, {"command": "echo hi"})
        self.assertEqual(step.thought, "先探一下")
        self.assertAlmostEqual(step.confidence, 0.3)

    def test_confidence_is_clamped(self):
        p, _ = self._planner([_call("shell", {"command": "x",
                                              "_confidence": 5})])
        step = p.plan("t", [], [])
        self.assertEqual(step.confidence, 1.0)

    def test_finish_tool(self):
        p, _ = self._planner([_call(FINISH, {
            "answer": "根因在 88 行", "unknowns": ["未确认调用路径"],
            "confidence": 0.8})])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, Finish)
        self.assertEqual(step.answer, "根因在 88 行")
        self.assertEqual(step.unknowns, ["未确认调用路径"])

    def test_ask_user_tool(self):
        p, _ = self._planner([_call(ASK_USER, {
            "question": "用哪个分支?", "options": ["main", "dev"]})])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, AskUser)
        self.assertEqual(step.options, ["main", "dev"])

    # ---- 失败路径 ----

    def test_malformed_json_is_bad_step(self):
        """最高频故障，且必须可恢复。"""
        p, _ = self._planner([_call("shell", "{command: echo")])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, BadStep)
        self.assertIn("JSON", step.reason)

    def test_unknown_tool_is_bad_step(self):
        p, _ = self._planner([_call("nope", {"a": 1})])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, BadStep)
        self.assertIn("可用", step.reason)

    def test_transport_error_is_bad_step(self):
        """模型服务挂了不许终结整个 loop。"""
        p, _ = self._planner([LLMError("HTTP 500")])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, BadStep)
        self.assertIn("模型调用失败", step.reason)

    def test_missing_name_is_bad_step(self):
        p, _ = self._planner([_call("", {})])
        self.assertIsInstance(p.plan("t", [], []), BadStep)

    # ---- 文本回退路径 ----

    def test_json_block_fallback(self):
        p, _ = self._planner([_text(
            "我先看看环境。\n```json\n"
            '{"tool": "shell", "args": {"command": "echo hi"}}\n```')])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, Action)
        self.assertEqual(step.args["command"], "echo hi")

    def test_finish_from_text_json(self):
        p, _ = self._planner([_text(
            '{"answer": "查清了", "unknowns": ["x"], "confidence": 0.9}')])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, Finish)
        self.assertEqual(step.unknowns, ["x"])

    def test_plain_text_becomes_finish(self):
        p, _ = self._planner([_text("我认为是空指针")])
        step = p.plan("t", [], [])
        self.assertIsInstance(step, Finish)
        self.assertEqual(step.answer, "我认为是空指针")
        self.assertEqual(step.unknowns, [])

    # ---- 请求构造 ----

    def test_tools_include_pseudo_tools_and_drop_risk(self):
        p, t = self._planner([_text("done")])
        p.plan("t", [], [])
        names = [x["name"] for x in t.calls[0]["tools"]]
        self.assertIn("shell", names)
        self.assertIn(ASK_USER, names)
        self.assertIn(FINISH, names)
        for tool in t.calls[0]["tools"]:
            self.assertNotIn("risk", tool,
                             "risk 是策略层字段，不该发给模型")

    def test_steering_injected_as_user_message(self):
        p, t = self._planner([_text("done")])
        p.plan("t", [], ["别删东西"])
        msgs = t.calls[0]["messages"]
        self.assertTrue(any("别删东西" in m.get("content", "")
                            for m in msgs if m["role"] == "user"))

    def test_history_is_capped(self):
        """轨迹会无限变长，早期失败尝试对当前决策几乎没价值。"""
        p, t = self._planner([_text("done")])
        p.max_history_turns = 2
        from zagent.loop import Turn
        from zagent.tools.base import ToolResult
        hist = [Turn(index=i, step=Action("shell", {"command": f"c{i}"}),
                     observation=ToolResult(ok=True, output="x"))
                for i in range(10)]
        p.plan("t", hist, [])
        tool_msgs = [m for m in t.calls[0]["messages"] if m["role"] == "tool"]
        self.assertEqual(len(tool_msgs), 2)

    def test_observation_is_clipped(self):
        p, t = self._planner([_text("done")])
        p.max_observation_chars = 50
        from zagent.loop import Turn
        from zagent.tools.base import ToolResult
        hist = [Turn(index=0, step=Action("shell", {"command": "c"}),
                     observation=ToolResult(ok=True, output="x" * 500))]
        p.plan("t", hist, [])
        tool_msg = [m for m in t.calls[0]["messages"] if m["role"] == "tool"][0]
        self.assertIn("截断", tool_msg["content"])

    def test_usage_logged(self):
        r = LLMResponse(content="done", usage={"total_tokens": 42})
        p, _ = self._planner([r])
        p.plan("t", [], [])
        self.assertEqual(p.stats()["usage"]["total_tokens"], 42)


class JsonExtractionTest(unittest.TestCase):
    def test_fenced(self):
        self.assertEqual(_extract_json('```json\n{"a": 1}\n```'), {"a": 1})

    def test_bare(self):
        self.assertEqual(_extract_json('前缀 {"a": 1} 后缀'), {"a": 1})

    def test_broken_returns_none(self):
        self.assertIsNone(_extract_json('{"a": '))

    def test_no_json_returns_none(self):
        self.assertIsNone(_extract_json("没有 JSON"))

    def test_finish_from_non_dict(self):
        self.assertIsInstance(_finish_from_text("随便说点什么"), Finish)


class LoopWithLLMTest(unittest.TestCase):
    """端到端：真 loop + 假模型。"""

    def _reg(self) -> ToolRegistry:
        reg = ToolRegistry()
        reg.register(make_shell_tool(timeout=20))
        return reg

    def test_full_run(self):
        reg = self._reg()
        transport = FakeTransport([
            _call("shell", {"command": f'"{PY}" -c "print(42)"'}),
            _call(FINISH, {"answer": "环境正常", "unknowns": []}),
        ])
        planner = LLMPlanner(reg, transport=transport)
        out = AgentLoop(reg, planner, max_steps=8).run("检查环境")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.answer, "环境正常")
        self.assertEqual(out.steps, 2)
        self.assertTrue(out.turns[0].observation.ok)

    def test_bad_step_is_survivable(self):
        """畸形输出后能自我纠正 —— 这正是 BadStep 存在的意义。"""
        reg = self._reg()
        transport = FakeTransport([
            _call("shell", "{broken"),
            _call("shell", {"command": f'"{PY}" -c "print(1)"'}),
            _call(FINISH, {"answer": "纠正成功"}),
        ])
        planner = LLMPlanner(reg, transport=transport)
        out = AgentLoop(reg, planner, max_steps=8).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertIsInstance(out.turns[0].step, BadStep)
        self.assertTrue(out.turns[1].observation.ok)

    def test_repeated_bad_steps_stall(self):
        """但不能无限纠正下去 —— 连续失败仍然要停。"""
        reg = self._reg()
        transport = FakeTransport([_call("shell", "{broken")] * 10)
        planner = LLMPlanner(reg, transport=transport)
        out = AgentLoop(reg, planner, max_steps=20).run("t")
        self.assertEqual(out.status, STATUS_STALLED)

    def test_low_confidence_triggers_approval(self):
        """模型说没把握，系统真的去问人 —— 不确定性进了控制流。"""
        reg = self._reg()
        transport = FakeTransport([
            _call("shell", {"command": "rm -rf build", "_confidence": 0.1}),
            _call(FINISH, {"answer": "未执行"}),
        ])
        planner = LLMPlanner(reg, transport=transport)
        loop = AgentLoop(reg, planner, policy=Policy(mode=Mode.NORMAL),
                         interactor=AutoInteractor(approve_default=False),
                         max_steps=8)
        out = loop.run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertFalse(out.turns[0].observation.ok)


class ConfigTest(unittest.TestCase):
    def test_defaults_are_deterministic(self):
        """temperature=0 是 eval 可复现的前提。"""
        self.assertEqual(LLMConfig().temperature, 0.0)

    def test_system_prompt_states_honesty_rule(self):
        """诚实性约束必须真的写进 prompt，不能只在代码里。"""
        self.assertIn("unknowns", SYSTEM_PROMPT)
        self.assertIn("没有证据", SYSTEM_PROMPT)


if __name__ == "__main__":
    unittest.main()
