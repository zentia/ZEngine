"""
结论硬门禁的回归测试。

    cd agentkit
    python -m unittest tests.test_gate -v

这些测试要回答的唯一问题是：**模型能不能绕过它**。

门禁的不可绕过性来自一个结构性事实：允许收尾的结论集合 = 轨迹里
被规则判为 CONFIRMED 且带可回溯证据的 claim，而模型无法自己往这个
集合里加东西。所以"凭空写一条结论"必然匹配不上，必然被拒。
下面 `test_fabricated_conclusion_is_rejected` 就是这条性质的直接验证。
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tests.test_core import StubCode

from zagent.gate import (Claim, ConclusionGate, STATUS_GATE_REJECTED,
                         collect_claims, matches)
from zagent.loop import (Action, AgentLoop, AutoInteractor, Finish, RunOutcome,
                         ScriptedPlanner, Turn, STATUS_DONE, STATUS_STALLED)
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry
from zagent.tools.base import ToolResult
from zagent.tools.verify_tool import make_verify_tools
from zagent.types import Evidence, Hypothesis, Verdict
from zagent.verify import RuleVerifier

VERIFIED = "a 在 actor.cpp:2 处使用前缺少空判断"

UNSAFE = "\n".join([
    "void WriteNameUnsafe(Actor* a) {",
    "    a->name[0] = 'x';",
    "}",
])
SAFE = "\n".join([
    "void WriteNameSafe(Actor* a) {",
    "    if (!a) return;",
    "    a->name[0] = 'x';",
    "}",
])


def _turn(data: dict) -> Turn:
    """造一条带观测的轨迹项。"""
    return Turn(index=0, step=Action("verify_hypothesis", {}),
                observation=ToolResult(ok=True, output="", data=data))


def _verified(verdict: str, with_evidence: bool = True) -> Turn:
    return _turn({
        "hypothesis": Hypothesis(claim=VERIFIED, kind="nullcheck"),
        "verdict": verdict,
        "evidence": ["actor.cpp:2"] if with_evidence else [],
    })


class ClaimCollectionTest(unittest.TestCase):
    def test_picks_up_single_verdict_shape(self):
        claims = collect_claims([_verified("confirmed")])
        self.assertEqual(len(claims), 1)
        self.assertTrue(claims[0].allowed)

    def test_picks_up_multi_conclusions_shape(self):
        """
        第二种形状：`data["conclusions"]`（多假设工具用，如 diagnose_crash）。

        门禁靠扫描观测工作，不认识具体工具 —— 将来加新能力不用改门禁。
        这里验证两种形状都能进台账。
        """
        claims = collect_claims([_turn({
            "conclusions": [
                {"claim": "X", "verdict": "confirmed", "locators": ["a.cpp:1"]},
                {"claim": "Y", "verdict": "refuted", "locators": []},
            ]})])
        self.assertEqual([c.claim for c in claims], ["X", "Y"])
        self.assertTrue(claims[0].allowed)
        self.assertFalse(claims[1].allowed)

    def test_confirmed_without_evidence_is_not_allowed(self):
        """
        CONFIRMED 但没有可回溯证据 -> 不可进结论。

        CONFIRMED 是必要不充分条件。少了后一半，一条"我说它对"也能过关。
        """
        self.assertFalse(_verified("confirmed", with_evidence=False) and
                         collect_claims([_verified("confirmed", False)])[0].allowed)

    def test_unproven_not_allowed(self):
        self.assertFalse(collect_claims([_verified("unproven")])[0].allowed)


class MatchTest(unittest.TestCase):
    def test_exact(self):
        self.assertTrue(matches(VERIFIED, VERIFIED))

    def test_paraphrase(self):
        """
        措辞不同也要能对上，否则门禁过严会把正确结论也拒掉。

        "a 缺少空判断" 和 "a 在 actor.cpp:2 处使用前缺少空判断" 是同一条
        断言，但字符级 ratio 只有 ~0.48（长度差把分母撑大了）。
        这条是实测踩出来的，不是推演 —— 只靠 ratio 会让 agent 一直改
        一直被拒，最后 stalled。
        """
        self.assertTrue(matches("a 缺少空判断", VERIFIED))

    def test_paraphrase_does_not_accept_unrelated(self):
        """放宽到覆盖率之后，无关的简短断言仍然要被拒。"""
        self.assertFalse(matches("内存泄漏", VERIFIED))

    def test_unrelated(self):
        self.assertFalse(matches("根因是内存泄漏", VERIFIED))

    def test_short_fragment_does_not_match_everything(self):
        """短句不参与包含匹配，否则"是""没有"这种词能匹配一切。"""
        self.assertFalse(matches("是", VERIFIED))


class GateRuleTest(unittest.TestCase):
    def _gate(self, **kw) -> ConclusionGate:
        return ConclusionGate(**kw)

    def test_fabricated_conclusion_is_rejected(self):
        """核心：凭空写的结论必然被拒 —— 这是门禁存在的理由。"""
        d = self._gate().check(
            Finish(answer="根因是内存泄漏", conclusions=["根因是内存泄漏"]),
            [_verified("confirmed")])
        self.assertFalse(d.allowed)
        self.assertIn("没有已验证的证据支撑", d.reason)

    def test_verified_conclusion_is_allowed(self):
        d = self._gate().check(
            Finish(answer="ok", conclusions=[VERIFIED]),
            [_verified("confirmed")])
        self.assertTrue(d.allowed)

    def test_refuted_claim_in_conclusions_is_rejected(self):
        """
        把被推翻的假设写进结论 —— 最严重的一类，单独报。

        它说明模型没读懂 REFUTED 的含义（把它当成了"工具出错"）。
        这条必须优先于其他判据报出，否则模型收到的是"没有证据支撑"，
        它会去找证据来支撑一条**已知错误**的假设。
        """
        d = self._gate().check(
            Finish(answer="ok", conclusions=[VERIFIED]),
            [_verified("refuted")])
        self.assertFalse(d.allowed)
        self.assertIn("已被规则推翻", d.reason)

    def test_no_conclusions_is_allowed_by_default(self):
        """
        默认档位允许"不下结论就收尾"。

        这是必须的：`require_confirmed=0` 要能服务普通任务
        （"列一下文件"没有假设可言）。否则门禁会把这个 CLI 变成
        什么都做不了的东西。
        """
        d = self._gate().check(Finish(answer="有 9 个文件"), [])
        self.assertTrue(d.allowed)

    def test_strict_requires_unknowns_when_no_conclusion(self):
        """
        严格档位：既不结论也不承认不知道 -> 拒。

        这正是"不诚实"最常见的形态：给一段听起来专业的话，
        不承担任何可核查的断言。
        """
        g = self._gate(require_confirmed=1)
        self.assertFalse(
            g.check(Finish(answer="看起来像是内存问题"), []).allowed)
        self.assertTrue(
            g.check(Finish(answer="没查出来",
                           unknowns=["没有可验证的假设"]), []).allowed)

    def test_strict_accepts_verified_conclusion(self):
        g = self._gate(require_confirmed=1)
        self.assertTrue(g.check(
            Finish(answer="ok", conclusions=[VERIFIED]),
            [_verified("confirmed")]).allowed)


class LoopIntegrationTest(unittest.TestCase):
    """门禁装在 loop 上，不是 planner 上 —— 换决策者不能绕过。"""

    def _loop(self, script, gate=None, max_steps=8):
        reg = ToolRegistry()
        return AgentLoop(reg, ScriptedPlanner(script),
                         policy=Policy(mode=Mode.NORMAL),
                         interactor=AutoInteractor(),
                         max_steps=max_steps, gate=gate)

    def test_rejection_is_survivable(self):
        """
        被拒**不终结运行**：变成观测塞回轨迹，模型有机会自己改。

        直接终结（或抛异常）会让这一步的结论直接丢失，
        而"改一下再交一次"才是我们想要的。
        """
        out = self._loop(
            [Finish(answer="瞎编", conclusions=["瞎编"]),
             Finish(answer="算了，没查出来", unknowns=["无线索"])],
            gate=ConclusionGate()).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.steps, 2)
        self.assertEqual(out.turns[0].observation.status, STATUS_GATE_REJECTED)
        self.assertFalse(out.turns[0].observation.ok)

    def test_repeated_rejection_stalls(self):
        """一直不改 -> 停滞。不能让它在门禁上无限打转。"""
        bad = Finish(answer="瞎编", conclusions=["瞎编"])
        out = self._loop([bad] * 6, gate=ConclusionGate()).run("t")
        self.assertEqual(out.status, STATUS_STALLED)
        self.assertIn("门禁", out.answer)

    def test_no_gate_means_no_enforcement(self):
        """
        默认不装门禁时行为不变。

        这条保证既有 eval 的对照实验不受影响 —— 装了门禁再跑对照，
        数字一变就无法归因了。
        """
        out = self._loop([Finish(answer="随便说", conclusions=["随便说"])]).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.conclusions, ["随便说"])

    def test_conclusions_carried_into_outcome(self):
        out = self._loop(
            [Finish(answer="ok", conclusions=[VERIFIED])]).run("t")
        self.assertEqual(out.conclusions, [VERIFIED])


class EndToEndTest(unittest.TestCase):
    """真工具 + 真规则 + 门禁，走完整条链。"""

    def _reg(self, src: str) -> ToolRegistry:
        reg = ToolRegistry()
        for t in make_verify_tools(StubCode(src), RuleVerifier()):
            reg.register(t)
        return reg

    def test_verify_then_conclude_passes(self):
        reg = self._reg(UNSAFE)
        planner = ScriptedPlanner([
            Action("verify_hypothesis", {
                "kind": "nullcheck", "claim": VERIFIED,
                "data": {"path": "actor.cpp", "line": 2, "variable": "a"}}),
            Finish(answer="找到了", conclusions=[VERIFIED]),
        ])
        out = AgentLoop(reg, planner, policy=Policy(mode=Mode.NORMAL),
                        interactor=AutoInteractor(),
                        gate=ConclusionGate()).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.conclusions, [VERIFIED])

    def test_conclude_without_verify_is_blocked(self):
        """
        不验证就下结论 -> 被拦。模型无法伪造 CONFIRMED，所以绕不过去。
        """
        reg = self._reg(UNSAFE)
        planner = ScriptedPlanner([
            Finish(answer="我觉得是空指针", conclusions=["a 缺少空判断"]),
            Finish(answer="不装了", unknowns=["未验证"]),
        ])
        out = AgentLoop(reg, planner, policy=Policy(mode=Mode.NORMAL),
                        interactor=AutoInteractor(),
                        gate=ConclusionGate()).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.unknowns, ["未验证"])
        self.assertEqual(out.conclusions, [])

    def test_refuted_then_concluded_is_blocked(self):
        """规则判 REFUTED，模型仍把它写进结论 -> 拦。"""
        reg = self._reg(SAFE)
        planner = ScriptedPlanner([
            Action("verify_hypothesis", {
                "kind": "nullcheck", "claim": VERIFIED,
                "data": {"path": "actor.cpp", "line": 3, "variable": "a"}}),
            Finish(answer="还是空指针", conclusions=[VERIFIED]),
            Finish(answer="推翻了", unknowns=["该假设已被推翻"]),
        ])
        out = AgentLoop(reg, planner, policy=Policy(mode=Mode.NORMAL),
                        interactor=AutoInteractor(),
                        gate=ConclusionGate()).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertIn("已被规则推翻", out.turns[1].observation.error)
        self.assertEqual(out.conclusions, [])


if __name__ == "__main__":
    unittest.main()
