"""
构建/测试工具的回归测试。

    cd agentkit
    python -m unittest tests.test_build_tools -v

刻意守住的几条，都是"改坏了不会立刻报错、但会让门禁静默失效"的类型：

- **退出码 0 但没干活 -> UNPROVEN**。这是本工具存在的全部理由。
  若有人图省事改成"退出码 0 就 CONFIRMED"，门禁会照常放行，而它已经
  什么都拦不住了 —— 这种失效没有任何症状。
- **REFUTED / UNPROVEN 必须 ok=True**。回成 ok=False，模型会当成工具
  故障去重试，把正确的否定结论当成失败。
- **claim 要短**。写成长句会让"构建通过"这种短声明匹配不上，造成
  "明明跑了构建却被驳回"。
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.gate import Claim, collect_claims, matches
from zagent.llm import FakeTransport, LLMPlanner, LLMResponse
from zagent.loop import AgentLoop, AutoInteractor, STATUS_DONE, STATUS_STALLED
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry
from zagent.tools.base import ToolResult
from zagent.tools.build import (BuildRun, build_evidence, make_build_tools,
                                test_evidence)

ROOT = Path(__file__).resolve().parent.parent


def runner_of(returncode: int, output: str, status: str = "ok"):
    def _r(command: str, cwd: str, timeout: float) -> BuildRun:
        return BuildRun(returncode=returncode, output=output, status=status,
                        produced_output=bool(output))
    return _r


class EvidenceTest(unittest.TestCase):
    def test_zero_tests_is_not_evidence(self):
        """
        0 个用例通过 -> 不是证据。

        最危险的假成功：退出码 0、什么都没测。若这里返回"0 个用例"当成
        证据，agent 就能靠一个空测试套件宣称"测试通过"。
        """
        self.assertIsNone(test_evidence("0 passed"))
        self.assertIsNone(test_evidence("collected 0 items"))

    def test_positive_tests_is_evidence(self):
        self.assertIsNotNone(test_evidence("42 passed"))
        self.assertIsNotNone(test_evidence("[  PASSED  ] 7 tests"))

    def test_no_op_build_is_not_evidence(self):
        self.assertIsNone(build_evidence("Nothing to be done"))
        self.assertIsNone(build_evidence(""))

    def test_real_build_is_evidence(self):
        self.assertIsNotNone(build_evidence("编译 128 个目标\n构建成功"))


class VerdictTest(unittest.TestCase):
    def _tools(self, runner):
        reg = ToolRegistry()
        for t in make_build_tools(root=ROOT, runner=runner):
            reg.register(t)
        return reg

    def test_exit_zero_with_evidence_is_confirmed(self):
        reg = self._tools(runner_of(0, "42 passed"))
        r = reg.call("run_tests")
        self.assertEqual(r.data["verdict"], "confirmed")
        self.assertTrue(r.ok)

    def test_nonzero_is_refuted(self):
        reg = self._tools(runner_of(2, "error C2065: undeclared"))
        r = reg.call("build_project")
        self.assertEqual(r.data["verdict"], "refuted")
        # REFUTED 不是工具故障。回成 ok=False 会让模型去重试，
        # 恰恰是把"构建确实失败了"这个正确结论当成失败。
        self.assertTrue(r.ok)

    def test_exit_zero_without_evidence_is_unproven(self):
        """
        退出码 0 但读不出证据 -> UNPROVEN，且**说清为什么**。

        这条是本工具存在的全部理由。改成"退出码 0 就 confirmed"，
        门禁依然会放行 —— 但它已经什么都拦不住了，而且没有任何症状。
        """
        reg = self._tools(runner_of(0, "Nothing to be done"))
        r = reg.call("build_project")
        self.assertEqual(r.data["verdict"], "unproven")
        self.assertTrue(r.ok)
        self.assertIn("证据", r.output)

    def test_unproven_cannot_enter_conclusions(self):
        """UNPROVEN 的 claim 不许进结论 —— 由 Claim.allowed 保证。"""
        reg = self._tools(runner_of(0, "0 passed"))
        r = reg.call("run_tests")
        self.assertEqual(r.data["verdict"], "unproven")
        for c in r.data["conclusions"]:
            claim = Claim(claim=c["claim"], verdict=c["verdict"],
                          locators=c["locators"])
            self.assertFalse(claim.allowed)

    def test_claim_is_short_and_has_locator(self):
        """
        claim 短、locators 非空。

        claim 写成长句（"构建成功（zbuild.py build，退出码 0）"），
        "构建通过"这种短声明会匹配不上 —— 明明跑了构建却被驳回。
        """
        reg = self._tools(runner_of(0, "42 passed"))
        r = reg.call("run_tests")
        c = r.data["conclusions"][0]
        self.assertEqual(c["claim"], "测试通过")
        self.assertTrue(c["locators"])
        self.assertTrue(matches("测试通过", c["claim"]))

    def test_ledger_records_everything(self):
        reg = self._tools(runner_of(0, "42 passed"))
        self.assertIn("还没有跑过", reg.call("verification_ledger").output)
        reg.call("run_tests")
        out = reg.call("verification_ledger").output
        self.assertIn("测试通过", out)


class GateIntegrationTest(unittest.TestCase):
    """
    端到端：这是整个设计的落点 —— 门禁不认识 build_project，
    却能在"没跑构建就宣称构建通过"时把 agent 拦下来。
    """

    def _run(self, script, runner, gate):
        reg = ToolRegistry()
        for t in make_build_tools(root=ROOT, runner=runner):
            reg.register(t)
        return AgentLoop(
            reg, LLMPlanner(reg, transport=FakeTransport(script)),
            policy=Policy(mode=Mode.NORMAL),
            interactor=AutoInteractor(approve_default=False),
            max_steps=8, gate=gate).run("加个功能并验证")

    @staticmethod
    def _finish(conclusions):
        return LLMResponse(tool_calls=[
            {"id": "c1", "type": "function",
             "function": {"name": "finish",
                          "arguments": '{"answer": "完成", "conclusions": '
                                       f'{conclusions!r}'.replace("'", '"')
                                       + ', "unknowns": []}'}}])

    @staticmethod
    def _call(name):
        return LLMResponse(tool_calls=[
            {"id": "c1", "type": "function",
             "function": {"name": name, "arguments": "{}"}}])

    def test_claiming_without_building_is_blocked(self):
        from zagent.gate import ConclusionGate
        out = self._run([self._finish(["构建通过"])],
                        runner_of(0, "42 passed"), ConclusionGate())
        self.assertEqual(out.status, STATUS_STALLED)

    def test_building_then_claiming_passes(self):
        """门禁不阻止诚实的工作：跑了构建，同样的声明就能过。"""
        from zagent.gate import ConclusionGate
        out = self._run([self._finish(["构建通过"]),
                         self._call("build_project"),
                         self._finish(["构建通过"])],
                        runner_of(0, "编译 128 个目标\n构建成功"),
                        ConclusionGate())
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.conclusions, ["构建通过"])

    def test_no_op_build_still_blocked(self):
        """跑了构建，但构建无事可做 -> 依然拦住。"""
        from zagent.gate import ConclusionGate
        out = self._run([self._finish(["构建通过"]),
                         self._call("build_project"),
                         self._finish(["构建通过"])],
                        runner_of(0, "Nothing to be done"),
                        ConclusionGate())
        self.assertEqual(out.status, STATUS_STALLED)


if __name__ == "__main__":
    unittest.main()
