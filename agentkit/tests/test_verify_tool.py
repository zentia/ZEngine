"""
Verifier 工具化的回归测试。

    cd agentkit
    python -m unittest tests.test_verify_tool -v

重点不是"能不能调用规则"，而是三个语义有没有做对：

1. REFUTED 必须是 ok=True —— 否则模型会把"假设不成立"当成工具故障去重试
2. UNPROVEN 必须是 ok=True 但明确不可进结论
3. RULE_PARAMS 不能和 RuleVerifier 的实际规则表脱节
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tests.test_core import StubCode

from zagent.loop import Action, AgentLoop, AutoInteractor, Finish, ScriptedPlanner
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry
from zagent.tools.base import STATUS_BAD_ARGS
from zagent.tools.verify_tool import RULE_PARAMS, make_verify_tools
from zagent.types import Verdict
from zagent.verify import RuleVerifier

# 缺少空判断：a 直接使用
UNSAFE = "\n".join([
    "void WriteNameUnsafe(Actor* a) {",
    "    a->name[0] = 'x';",
    "}",
])

# 有空判断：第 2 行保护了 a
SAFE = "\n".join([
    "void WriteNameSafe(Actor* a) {",
    "    if (!a) return;",
    "    a->name[0] = 'x';",
    "}",
])


def _reg(src: str) -> ToolRegistry:
    code = StubCode(src)
    reg = ToolRegistry()
    for t in make_verify_tools(code, RuleVerifier()):
        reg.register(t)
    return reg


class VerifySemanticsTest(unittest.TestCase):
    def test_confirmed_for_missing_null_check(self):
        r = _reg(UNSAFE).call("verify_hypothesis", {
            "kind": "nullcheck",
            "claim": "a 在使用前缺少空判断",
            "data": {"path": "actor.cpp", "line": 2, "variable": "a"},
        })
        self.assertTrue(r.ok)
        self.assertEqual(r.status, "confirmed")
        self.assertEqual(r.data["verdict"], Verdict.CONFIRMED.value)
        self.assertTrue(r.data["allowed_in_conclusion"])
        self.assertTrue(r.data["evidence"], "CONFIRMED 必须带可回溯证据")

    def test_refuted_is_ok_not_error(self):
        """
        最关键的一条：REFUTED 是**成功验证**的结果，不是失败。

        回成 ok=False 的话，模型会把它当成"工具出错"去重试同一条假设 ——
        恰恰是把正确结论当成故障，而且会死循环。
        """
        r = _reg(SAFE).call("verify_hypothesis", {
            "kind": "nullcheck",
            "claim": "a 在使用前缺少空判断",
            "data": {"path": "actor.cpp", "line": 3, "variable": "a"},
        })
        self.assertTrue(r.ok, "REFUTED 也必须 ok=True")
        self.assertEqual(r.status, "refuted")
        self.assertFalse(r.data["allowed_in_conclusion"])
        self.assertIn("REFUTED".lower(), r.output.lower())

    def test_unproven_is_ok_but_not_concludable(self):
        """无法判定也是成功完成，但绝不能进结论。"""
        r = _reg(UNSAFE).call("verify_hypothesis", {
            "kind": "nullcheck",
            "claim": "越界行",
            "data": {"path": "actor.cpp", "line": 999, "variable": "a"},
        })
        self.assertTrue(r.ok)
        self.assertEqual(r.status, "unproven")
        self.assertFalse(r.data["allowed_in_conclusion"])

    def test_unknown_kind_rejected(self):
        r = _reg(UNSAFE).call("verify_hypothesis", {"kind": "vibes", "claim": "x"})
        self.assertEqual(r.status, STATUS_BAD_ARGS)
        self.assertIn("未知假设类型", r.error)

    def test_missing_params_rejected(self):
        """
        参数不齐必须在工具层就拦掉。

        放行会怎样：`from_data` 返回 None -> 规则判 UNPROVEN。
        模型看到 UNPROVEN 往往以为是"这条假设不成立"，于是换一条 ——
        而真实原因只是它少传了一个参数。错误的原因比错误的答案更耽误事。
        """
        r = _reg(UNSAFE).call("verify_hypothesis", {
            "kind": "nullcheck", "claim": "x", "data": {"path": "a.cpp"}})
        self.assertEqual(r.status, STATUS_BAD_ARGS)
        self.assertIn("variable", r.error)

    def test_rule_execution_error_is_observation(self):
        """规则内部炸了也要变成观测，不能掀翻工具层。"""

        class Bad:
            def check(self, h, code):
                raise RuntimeError("boom")

        code = StubCode(UNSAFE)
        reg = ToolRegistry()
        for t in make_verify_tools(code, Bad()):
            reg.register(t)
        r = reg.call("verify_hypothesis", {
            "kind": "nullcheck", "claim": "x",
            "data": {"path": "a.cpp", "line": 2, "variable": "a"}})
        self.assertFalse(r.ok)
        self.assertIn("boom", r.error)


class RuleParamConsistencyTest(unittest.TestCase):
    def test_params_match_verifier_rules(self):
        """
        RULE_PARAMS 不能和 RuleVerifier 的实际规则表脱节。

        新增一条规则却忘记登记参数，工具就会一直返回"未知假设类型"，
        而**没有任何测试会失败** —— 这属于"静默失效"，是最难发现的一类。
        这条断言把它变成显式的构建期错误。
        """
        self.assertEqual(set(RULE_PARAMS), set(RuleVerifier().kinds()))

    def test_every_kind_has_hint(self):
        from zagent.tools.verify_tool import RULE_HINT
        self.assertEqual(set(RULE_HINT), set(RULE_PARAMS))

    def test_list_rules_lists_all(self):
        r = _reg(UNSAFE).call("list_verify_rules", {})
        self.assertTrue(r.ok)
        self.assertEqual(len(r.data["kinds"]), len(RULE_PARAMS))
        for kind in RULE_PARAMS:
            self.assertIn(kind, r.output)


class LedgerTest(unittest.TestCase):
    def test_ledger_keeps_refuted(self):
        """
        被推翻的也要留着。

        这是"可归因"的前提：事后能回答"它试过什么、为什么否掉"。
        只留成功的台账会让复盘变成自证。
        """
        reg = _reg(SAFE)
        reg.call("verify_hypothesis", {
            "kind": "nullcheck", "claim": "a 缺空判断",
            "data": {"path": "actor.cpp", "line": 3, "variable": "a"}})
        r = reg.call("conclusion_ledger", {})
        self.assertEqual(len(r.data["refuted"]), 1)
        self.assertEqual(r.data["confirmed"], [])

    def test_ledger_empty_initially(self):
        r = _reg(UNSAFE).call("conclusion_ledger", {})
        self.assertTrue(r.ok)
        self.assertIn("还没有验证过", r.output)

    def test_ledger_accumulates_across_calls(self):
        """台账在同一会话内累积 —— 它是收尾自检的依据。"""
        reg = _reg(UNSAFE)
        reg.call("verify_hypothesis", {
            "kind": "nullcheck", "claim": "缺空判断",
            "data": {"path": "actor.cpp", "line": 2, "variable": "a"}})
        reg.call("verify_hypothesis", {
            "kind": "nullcheck", "claim": "缺空判断",
            "data": {"path": "actor.cpp", "line": 999, "variable": "a"}})
        r = reg.call("conclusion_ledger", {})
        self.assertEqual(len(r.data["confirmed"]), 1)
        self.assertEqual(len(r.data["unproven"]), 1)


class LoopIntegrationTest(unittest.TestCase):
    """验证工具在 loop 里是可用的，且 REFUTED 不会触发停滞。"""

    def test_refuted_does_not_stall(self):
        reg = _reg(SAFE)
        planner = ScriptedPlanner([
            Action("verify_hypothesis", {
                "kind": "nullcheck", "claim": "a 缺空判断",
                "data": {"path": "actor.cpp", "line": 3, "variable": "a"}}),
            Finish(answer="这条假设被推翻，换一条"),
        ])
        out = AgentLoop(reg, planner, policy=Policy(mode=Mode.NORMAL),
                        interactor=AutoInteractor(), max_steps=6).run("t")
        self.assertEqual(out.status, "done")
        self.assertTrue(out.turns[0].observation.ok)


if __name__ == "__main__":
    unittest.main()
