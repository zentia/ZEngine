"""
CLI / 崩溃工具 / agent 级 eval 的回归测试。

    cd agentkit
    python -m unittest tests.test_agent_cli -v

覆盖三件事：
1. `diagnose()` 真的被降级成了工具，且**不偷偷切分支**；
2. 没有模型时 CLI 仍可用（规则路由），且如实声明自己在降级；
3. agent 级 eval 能取出 `Report`，从而与流水线基线共用一把尺子。
"""

from __future__ import annotations

import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tests.test_core import StubCode  # 复用既有的最小代码索引

from zagent.eval import Sample, SampleSet
from zagent.eval_agent import (AgentRunStats, evaluate_agent,
                               run_agent_on_sample)
from zagent.loop import (Action, AgentLoop, AskUser, AutoInteractor, Finish,
                         ScriptedPlanner)
from zagent.policy import Mode, Policy
from zagent.protocols import Context
from zagent.tools import ToolRegistry, make_shell_tool
from zagent.tools.base import STATUS_BAD_ARGS, STATUS_ERROR, ToolResult
from zagent.tools.crash import make_crash_tools
from zagent.types import Facts, Revision, Report

from zagent.cli import RulePlanner, _clean, build_registry
from zagent.cli import QueueInteractor

SRC = "\n".join(f"line {i}" for i in range(1, 40))


class _Src:
    def __init__(self, facts: Facts | None = None) -> None:
        self._facts = facts or Facts(ref="r", platform="Windows",
                                     product_version="1.0")

    def ready(self) -> bool:
        return True

    def fetch(self, ref: str) -> Facts:
        self._facts.ref = ref
        return self._facts


class _DeadSrc:
    def ready(self) -> bool:
        return False

    def fetch(self, ref):
        raise AssertionError("不该被调用")


class _Version:
    def resolve(self, product_version: str):
        return Revision(id="r1")


def _ctx(src=None, code=None) -> Context:
    code = code or StubCode(SRC, rev="r1")
    return Context(source=src or _Src(), version=_Version(), code=code,
                   reasoner=None, verifier=None)


class CrashToolTest(unittest.TestCase):
    def _reg(self, ctx: Context | None) -> ToolRegistry:
        reg = ToolRegistry()
        for t in make_crash_tools(lambda ref: ctx):
            reg.register(t)
        return reg

    def test_report_is_exposed_in_data(self):
        """Report 对象必须可取回 —— eval 复用既有 score 全靠它。"""
        reg = self._reg(_ctx())
        r = reg.call("diagnose_crash", {"ref": "abc"})
        self.assertIsInstance(r.data.get("report"), Report)

    def test_empty_ref_rejected(self):
        reg = self._reg(_ctx())
        self.assertEqual(reg.call("diagnose_crash", {"ref": ""}).status,
                         STATUS_BAD_ARGS)

    def test_unresolvable_ctx_is_error(self):
        reg = self._reg(None)
        self.assertEqual(reg.call("diagnose_crash", {"ref": "x"}).status,
                         STATUS_ERROR)

    def test_missing_capability_blocked(self):
        ctx = Context(source=_DeadSrc(), version=_Version(),
                      code=StubCode(SRC))
        reg = self._reg(ctx)
        r = reg.call("diagnose_crash", {"ref": "x"})
        self.assertEqual(r.status, "blocked")
        self.assertFalse(r.ok)
        self.assertIn("source", r.data["missing"])

    def test_never_auto_checkout(self):
        """
        诊断工具绝不自动切分支。

        自动切分支会改动开发者的 working tree，那是破坏性操作。
        它该由 agent 显式调 shell 去做并走审批，不能藏在一个"只读诊断"
        工具里偷偷做掉。**测试里必须有这条断言**，否则将来谁顺手把
        auto_checkout 改成 True，护栏就悄悄没了。
        """
        code = StubCode(SRC, rev="r1")

        def _explode(rev, allow_dirty=True):
            raise AssertionError("诊断工具不应触发 checkout")

        code.checkout = _explode  # type: ignore[method-assign]
        reg = self._reg(_ctx(code=code))
        reg.call("diagnose_crash", {"ref": "x"})   # 不抛即通过

    def test_risk_is_read(self):
        reg = self._reg(_ctx())
        self.assertEqual(reg.get("diagnose_crash").risk.value, "read")

    def test_ready_tool(self):
        reg = self._reg(_ctx())
        r = reg.call("crash_ready", {})
        self.assertTrue(r.ok)
        self.assertEqual(r.data["missing"], [])


class RulePlannerTest(unittest.TestCase):
    def setUp(self):
        self.reg = ToolRegistry()
        for t in make_crash_tools(lambda ref: _ctx()):
            self.reg.register(t)

    def _p(self) -> RulePlanner:
        return RulePlanner(self.reg)

    def test_routes_crash_to_diagnose(self):
        step = self._p().plan("帮我定位这次崩溃 /tmp/a.dmp", [], [])
        self.assertIsInstance(step, Action)
        self.assertEqual(step.tool, "diagnose_crash")
        self.assertEqual(step.args["ref"], "/tmp/a.dmp")

    def test_asks_when_ref_missing(self):
        """信息不足时提问，而不是拿一个空 ref 去撞工具。"""
        step = self._p().plan("这次崩溃是什么原因", [], [])
        self.assertIsInstance(step, AskUser)

    def test_unknown_intent_declares_limit(self):
        """降级时必须如实声明，不能假装自己有推理能力。"""
        step = self._p().plan("今天天气怎么样", [], [])
        self.assertIsInstance(step, Finish)
        self.assertTrue(step.unknowns)

    def test_finishes_after_observation(self):
        from zagent.loop import Turn
        hist = [Turn(index=0, step=Action("diagnose_crash", {"ref": "x"}),
                     observation=ToolResult(ok=True, output="定位到 x.cpp:22"))]
        step = self._p().plan("t", hist, [])
        self.assertIsInstance(step, Finish)
        self.assertIn("x.cpp:22", step.answer)
        self.assertTrue(step.unknowns, "规则路由必须声明自己只走了一步")


class CliHelpersTest(unittest.TestCase):
    def test_bom_stripped(self):
        """BOM 会让 /exit 不再以 / 开头，从而被当成普通任务执行。"""
        self.assertEqual(_clean("\ufeff/exit"), "/exit")
        self.assertEqual(_clean("  hi  "), "hi")

    def test_build_registry_always_has_shell(self):
        reg = build_registry(Path(".").resolve())
        self.assertIn("shell", reg.names())

    def test_build_registry_without_crash_ctx(self):
        reg = build_registry(Path(".").resolve())
        self.assertNotIn("diagnose_crash", reg.names())

    def test_build_registry_with_crash_ctx(self):
        reg = build_registry(Path(".").resolve(),
                             resolve_crash_ctx=lambda ref: _ctx())
        self.assertIn("diagnose_crash", reg.names())


class QueueInteractorTest(unittest.TestCase):
    def test_ask_blocks_until_answered(self):
        """
        验证"agent 阻塞等答案、主线程回传"这条通路。

        这是交互式 CLI 的核心：两个线程不能同时读 stdin，否则互相吃输入。
        """
        it = QueueInteractor()
        got: list[str] = []

        def _worker():
            got.append(it.ask(AskUser(question="用哪个分支?",
                                      options=["main", "dev"])))

        th = threading.Thread(target=_worker)
        th.start()

        pend = None
        for _ in range(100):
            pend = it.pending()
            if pend:
                break
            th.join(timeout=0.02)
        self.assertIsNotNone(pend, "提问必须能被主线程取到")
        QueueInteractor.reply(pend[1], "main")
        th.join(timeout=2)
        self.assertEqual(got, ["main"])

    def test_approve_parsing(self):
        it = QueueInteractor()
        slot: dict = {}

        def _worker():
            slot["ok"] = it.approve("要删掉 build 吗?")

        th = threading.Thread(target=_worker)
        th.start()
        pend = None
        for _ in range(100):
            pend = it.pending()
            if pend:
                break
            th.join(timeout=0.02)
        QueueInteractor.reply(pend[1], "是")
        th.join(timeout=2)
        self.assertTrue(slot["ok"])


class AgentEvalTest(unittest.TestCase):
    """agent 级 eval：能否取出 Report 并与流水线共用尺子。"""

    def _reg(self, ref_holder: dict) -> ToolRegistry:
        reg = ToolRegistry()
        for t in make_crash_tools(lambda ref: _ctx()):
            reg.register(t)
        return reg

    def _build_loop(self, ref: str, with_report: bool = True):
        reg = self._reg({})
        if not with_report:
            reg = ToolRegistry()          # 没有诊断工具 -> 取不到报告
            reg.register(make_shell_tool())
        steps = ([Action("diagnose_crash", {"ref": ref}), Finish(answer="done")]
                 if with_report else [Finish(answer="没查")])
        return AgentLoop(reg, ScriptedPlanner(steps),
                         policy=Policy(mode=Mode.NORMAL),
                         interactor=AutoInteractor(), max_steps=6)

    def test_report_extracted(self):
        s = Sample(id="p1", ref="x.dmp", expected_path="x.cpp", expected_line=1)
        rep, st = run_agent_on_sample(s, lambda sm: self._build_loop(sm.ref))
        self.assertIsInstance(rep, Report)
        self.assertTrue(st.has_report)
        self.assertEqual(st.steps, 2)
        self.assertFalse(st.stalled)

    def test_missing_report_is_blocked_not_crash(self):
        """
        取不到报告时返回 blocked 空报告，**不能返回 None**。

        返回 None 会让 evaluate() 在 .top() 上炸，整个 eval 停在半路；
        而一个明确的 blocked 是可以进统计、可以归因的。
        """
        s = Sample(id="p2", ref="y.dmp", expected_path="x.cpp", expected_line=1)
        rep, st = run_agent_on_sample(
            s, lambda sm: self._build_loop(sm.ref, with_report=False))
        self.assertIsInstance(rep, Report)
        self.assertEqual(rep.status, "blocked")
        self.assertFalse(st.has_report)
        self.assertIn("agent 未产出诊断报告", rep.block_reason)

    def test_evaluate_agent_matches_pipeline_shape(self):
        """产出的 base 就是既有 EvalReport，指标口径与流水线一致。"""
        ss = SampleSet(name="t", samples=[
            Sample(id="p1", ref="x.dmp", expected_path="x.cpp", expected_line=1),
            Sample(id="n1", ref="n.dmp", negative=True),
        ])
        out = evaluate_agent(ss, lambda sm: self._build_loop(sm.ref))
        self.assertEqual(out.base.total, 2)
        self.assertEqual(len(out.runs), 2)
        self.assertGreaterEqual(out.avg_steps, 1.0)
        self.assertEqual(out.no_report_rate, 0.0)
        self.assertIn("agent 过程指标", out.render())


if __name__ == "__main__":
    unittest.main()
