"""
回归测试。用 unittest，不需要 pytest。

    cd agentkit
    python -m unittest discover -s tests -v

重点测的是三条硬约束：
1. 版本不一致必须阻塞，且绝不自动切代码
2. REFUTED 绝不进结论
3. 缺字段必须显式出现在 unknowns 而非被默认值掩盖
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.adapters import (GitCodeIndex, LocalDumpSource,  # noqa: E401
                            SimpleVersionResolver, find_git)
from zagent.eval import Sample, SampleSet, _normalize, evaluate, score
from zagent.pipeline import diagnose
from zagent.protocols import Context
from zagent.tracing import Tracer
from zagent.types import (Evidence, Facts, Hypothesis, Report, Revision,
                          SourceLocation, Verdict, compute_confidence)
from zagent.verify import (CallChainRule, HeuristicReasoner, NullCheckRule,
                           RuleVerifier)


class StubCode:
    """不依赖 git 的最小代码索引，用于隔离测试验证规则。"""

    def __init__(self, text: str, rev: str = "r1") -> None:
        self.text = text
        self.rev = rev
        self.lines = text.splitlines()

    def current_revision(self) -> Revision:
        return Revision(id=self.rev)

    def checkout(self, rev, allow_dirty=True):
        raise AssertionError("测试不应触发真实 checkout")

    def is_project_code(self, frame) -> bool:
        return True

    def map_to_local(self, symbol):
        return SourceLocation(path=Path(symbol.file or "x.cpp"),
                              line=symbol.line or 1, symbol=symbol.function)

    def read_lines(self, path: Path) -> list[str]:
        return list(self.lines)

    def read_context(self, loc, radius=50) -> str:
        lo = max(0, loc.line - 1 - radius)
        hi = min(len(self.lines), loc.line + radius)
        return "\n".join(f"{i+1}: {self.lines[i]}" for i in range(lo, hi))

    def grep(self, pattern, limit=200):
        import re
        rx = re.compile(pattern)
        return [(Path("x.cpp"), i + 1, l)
                for i, l in enumerate(self.lines) if rx.search(l)][:limit]


# ---------------------------------------------------------------- 类型与置信度

class TestConfidence(unittest.TestCase):
    def test_empty_conclusions_is_zero(self):
        self.assertEqual(compute_confidence([], []), 0.0)

    def test_unproven_only_is_low(self):
        h = Hypothesis(claim="x", kind="nullcheck")
        h.verdict = Verdict.UNPROVEN
        c = compute_confidence([h], [])
        self.assertLess(c, 0.3)

    def test_missing_fields_lower_confidence(self):
        h = Hypothesis(claim="x", kind="nullcheck")
        h.verdict = Verdict.CONFIRMED
        h.evidence.append(Evidence(method="read_code", locator="a.cpp:1"))
        high = compute_confidence([h], [])
        low = compute_confidence([h], ["stack", "uid", "platform"])
        self.assertLess(low, high)

    def test_untraceable_evidence_penalized(self):
        h = Hypothesis(claim="x", kind="nullcheck")
        h.verdict = Verdict.CONFIRMED
        h.evidence.append(Evidence(method="guess", locator="", detail="感觉是这里"))
        c = compute_confidence([h], [])
        self.assertLess(c, 0.6)

    def test_refuted_never_becomes_conclusion(self):
        h = Hypothesis(claim="x", kind="nullcheck")
        h.refute(Evidence(method="read_code", locator="a.cpp:1"))
        self.assertFalse(h.is_conclusion)


# ---------------------------------------------------------------- 验证规则

class TestNullCheckRule(unittest.TestCase):
    SRC = "\n".join([
        "void Safe(Actor* a)",      # 1
        "{",                        # 2
        "    if (a == nullptr)",    # 3
        "        return;",          # 4
        "    a->id = 1;",           # 5
        "}",                        # 6
        "void Unsafe(Actor* a)",    # 7
        "{",                        # 8
        "    a->id = 2;",           # 9
        "}",                        # 10
    ])

    def test_detects_missing_guard(self):
        code = StubCode(self.SRC)
        h = NullCheckRule(SourceLocation(path=Path("x.cpp"), line=9), "a").to_hypothesis()
        RuleVerifier().check(h, code)
        self.assertEqual(h.verdict, Verdict.CONFIRMED)

    def test_detects_existing_guard(self):
        code = StubCode(self.SRC)
        h = NullCheckRule(SourceLocation(path=Path("x.cpp"), line=5), "a").to_hypothesis()
        RuleVerifier().check(h, code)
        self.assertEqual(h.verdict, Verdict.REFUTED)

    def test_guard_in_other_function_not_counted(self):
        """上一个函数的保护不能被算到当前函数头上。"""
        code = StubCode(self.SRC)
        # 第 9 行（Unsafe 内）之前确实有第 3 行的保护，但它在 Safe 里
        h = NullCheckRule(SourceLocation(path=Path("x.cpp"), line=9), "a").to_hypothesis()
        RuleVerifier().check(h, code)
        self.assertEqual(h.verdict, Verdict.CONFIRMED,
                         "跨函数的空判断不应被计入")

    def test_unknown_kind_stays_unproven(self):
        code = StubCode(self.SRC)
        h = Hypothesis(claim="x", kind="no_such_rule")
        RuleVerifier().check(h, code)
        self.assertEqual(h.verdict, Verdict.UNPROVEN)


class TestCallChainRule(unittest.TestCase):
    SRC = "\n".join([
        "void Inner()",       # 1
        "{",                  # 2
        "}",                  # 3
        "void Outer()",       # 4
        "{",                  # 5
        "    Inner();",       # 6
        "}",                  # 7
    ])

    def test_confirms_real_call(self):
        code = StubCode(self.SRC)
        h = CallChainRule("Outer", "Inner").to_hypothesis()
        RuleVerifier().check(h, code)
        self.assertEqual(h.verdict, Verdict.CONFIRMED)

    def test_refutes_when_callee_absent(self):
        code = StubCode(self.SRC)
        h = CallChainRule("Outer", "NotExist").to_hypothesis()
        RuleVerifier().check(h, code)
        self.assertEqual(h.verdict, Verdict.REFUTED)


# ---------------------------------------------------------------- 流水线护栏

class StubSource:
    name = "stub"

    def __init__(self, facts):
        self.facts = facts

    def ready(self):
        return True

    def fetch(self, ref):
        return self.facts


class StubVersion:
    def __init__(self, rev):
        self.rev = rev

    def resolve(self, v):
        return Revision(id=self.rev)


class TestPipelineGuards(unittest.TestCase):
    def _ctx(self, facts, code_rev="r1", target_rev="r1"):
        code = StubCode("void f(){}\n", rev=code_rev)
        return Context(source=StubSource(facts), version=StubVersion(target_rev),
                       code=code, reasoner=HeuristicReasoner(code),
                       verifier=RuleVerifier())

    def test_version_mismatch_blocks(self):
        facts = Facts(product_version="1.0.0.2", stack=[])
        ctx = self._ctx(facts, code_rev="r1", target_rev="r2")
        rep = diagnose("x", ctx, Tracer("t"))
        self.assertEqual(rep.status, "blocked")
        self.assertIn("版本不一致", rep.block_reason)

    def test_no_auto_checkout_by_default(self):
        """默认绝不自动切代码。"""
        facts = Facts(product_version="1.0.0.2", stack=[])
        ctx = self._ctx(facts, code_rev="r1", target_rev="r2")
        rep = diagnose("x", ctx, Tracer("t"))
        self.assertEqual(rep.status, "blocked")

    def test_missing_fields_reach_unknowns(self):
        facts = Facts(product_version="1.0.0.1", stack=[])
        facts.note_missing("stack", "uid")
        ctx = self._ctx(facts)
        rep = diagnose("x", ctx, Tracer("t"))
        self.assertIn("stack", rep.unknowns)
        self.assertIn("uid", rep.unknowns)

    def test_gate_blocks_when_source_unavailable(self):
        class Dead:
            name = "dead"
            def ready(self): return False
            def fetch(self, ref): raise AssertionError("不该被调用")
        ctx = Context(source=Dead(), version=StubVersion("r1"),
                      code=StubCode("void f(){}"))
        rep = diagnose("x", ctx, Tracer("t"))
        self.assertEqual(rep.status, "blocked")
        self.assertIn("source", rep.block_reason)


# ---------------------------------------------------------------- eval

class TestEval(unittest.TestCase):
    def test_path_normalization(self):
        """相对路径与绝对路径必须归一到同一键。"""
        self.assertEqual(
            _normalize("src/actor.cpp:22"),
            _normalize("C:/tmp/repo/src/actor.cpp:22"),
        )

    def test_negative_sample_scored_by_restraint(self):
        s = Sample(id="n1", ref="", negative=True)
        rep = Report(status="blocked", block_reason="版本不一致")
        self.assertTrue(score(s, rep).honest)

        rep2 = Report(status="ok", confidence=0.9, conclusions=[Hypothesis(claim="x")])
        self.assertFalse(score(s, rep2).honest)


# ---------------------------------------------------------------- tracing

class TestTracing(unittest.TestCase):
    def test_flush_and_load(self):
        with tempfile.TemporaryDirectory() as d:
            t = Tracer("r1", Path(d))
            t.begin("gate"); t.end("gate", "ok")
            t.flush(outcome={"status": "ok"}, ledger=[])
            runs = Tracer.load(Path(d) / "traces.jsonl")
            self.assertEqual(len(runs), 1)
            self.assertEqual(runs[0]["stages"][0]["name"], "gate")

    def test_summarize_counts_verdicts(self):
        h = Hypothesis(claim="x", kind="k")
        h.verdict = Verdict.CONFIRMED
        with tempfile.TemporaryDirectory() as d:
            Tracer("r1", Path(d)).flush(outcome={"status": "ok", "confidence": 0.5},
                                        ledger=[h])
            s = Tracer.summarize(Tracer.load(Path(d) / "traces.jsonl"))
            self.assertEqual(s["verdicts"]["confirmed"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
from unittest import mock


class TestRevisionNeverFakedFromErrorText(unittest.TestCase):
    """拿不到 revision 必须返回空，不能把错误文本当版本号。

    这里锁住一个真实的坑：current_revision() 曾写成
    `Revision(id=out or "unknown")` 并忽略返回码，于是 git 不可用时
    "git not found" 这个字符串被当成了一个 revision。版本对齐门禁拿它
    和崩溃端的 "nogit" 比对，两边都是垃圾值、恰好不等，4 个合成样本
    全部被 block，top1 掉到 0%。

    教训：**错误信息不是数据**。拿不到就说拿不到（空 id），让门禁走
    "版本无法确定" 分支；把错误文本塞进数据字段，只会让它看起来像是有
    值，从而绕过所有缺失检查。
    """

    def test_git_missing_yields_empty_not_error_text(self):
        idx = GitCodeIndex(Path(r"E:\Engine\ZEngine"))
        with mock.patch.object(GitCodeIndex, "_git", return_value=(1, "git not found")):
            rev = idx.current_revision()
        self.assertEqual(rev.id, "")
        self.assertNotIn("git not found", rev.id)

    def test_git_empty_output_yields_empty(self):
        idx = GitCodeIndex(Path(r"E:\Engine\ZEngine"))
        with mock.patch.object(GitCodeIndex, "_git", return_value=(0, "")):
            self.assertEqual(idx.current_revision().id, "")

    def test_successful_revision_preserved(self):
        idx = GitCodeIndex(Path(r"E:\Engine\ZEngine"))
        with mock.patch.object(GitCodeIndex, "_git", return_value=(0, "abc123")):
            rev = idx.current_revision()
        self.assertEqual(rev.id, "abc123")


class TestFindGit(unittest.TestCase):
    """git 探测只该有一份实现。

    之前 adapters.py 和 examples/run_eval.py 各写一份降级
    （"git not found" / "nogit"），互不知情，门禁靠两边垃圾值恰好相等
    才放行 —— 等于没生效。
    """

    def test_returns_executable_or_none(self):
        r = find_git()
        self.assertTrue(r is None or isinstance(r, str))

    def test_no_error_text_leaks(self):
        """返回值要么是真路径，要么是 None，绝不可能是错误描述。"""
        r = find_git()
        if r is not None:
            self.assertNotIn("not found", r)
            self.assertNotIn("nogit", r)

