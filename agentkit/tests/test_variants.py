"""变体生成器测试。

重点不是"能生成"，而是**生成的东西真的成立**：能编译、能崩溃、标签对。
这三条每一条都是这一轮实际踩过的坑：

- 能编译：转发层引用不到 main 的局部变量 / argc -> 26 个变体跑不起来
- 能崩溃：main 压成单行导致探针装到函数体外面 -> 编译过但运行不崩
- 标签对：注入后行号漂移，回退到模板原行号 -> ground truth 指向错行

而且这三条失败**都不会报错**，只是让样本静默消失或标签错位，
指标看上去依然漂亮。所以它们必须被测试钉住。
"""

from __future__ import annotations

import unittest
from unittest import mock

from zfault import toolchain as T
from zfault.toolchain import Toolchain
from zfault.patterns import all_patterns, get
from zagent.eval import EvalReport, Sample, score
from zagent.types import Evidence, Hypothesis, Report, Verdict
from zfault.variants import (
    _find_fault_line,
    generate_clean_variants,
    generate_variants,
    make_variant,
)



class TestGeneration(unittest.TestCase):
    def test_generates_for_deterministic_only(self):
        """非确定性模板（race/overflow）不生成变体。

        它们是否崩溃本身随机，跑出来的"未崩溃"是样本性质不是生成器失效，
        混进来只会让有效样本率这个指标失真。
        """
        vs = generate_variants(per_pattern=2)
        self.assertTrue(vs)
        for v in vs:
            base = v.id.split(".")[0]
            self.assertTrue(get(base).deterministic,
                            "%s 非确定性却生成了变体" % base)

    def test_excludes_negatives(self):
        """负样本不参与变体：稀释正样本比例会让 top1 虚高。"""
        vs = generate_variants(per_pattern=2)
        for v in vs:
            self.assertNotIn("clean_01", v.id)
            self.assertNotIn("leak_01", v.id)

    def test_reproducible(self):
        a = generate_variants(per_pattern=3, seed=42)
        b = generate_variants(per_pattern=3, seed=42)
        self.assertEqual([v.id for v in a], [v.id for v in b])
        self.assertEqual([v.source for v in a], [v.source for v in b])

    def test_deterministic_patterns_present(self):
        vs = generate_variants(per_pattern=8)
        depths = {v.id.split(".")[1] for v in vs}
        # 维度轮换必须覆盖到 depth=3，否则"深度是否影响准确率"测不出来
        self.assertIn("d0n", depths)
        self.assertIn("d3y", depths)


class TestVariantSourceValidity(unittest.TestCase):
    """变体源码必须真的成立 —— 这三条每一条都曾静默失败过。"""

    def test_fault_marker_survives(self):
        """注入后必须还能找到根因标记，否则标签无从谈起。"""
        for p in all_patterns():
            if not p.deterministic:
                continue
            for d in (1, 2, 3):
                v = make_variant(p, depth=d, decoy=True, seed=1)
                if v is None:
                    continue
                self.assertGreater(_find_fault_line(v.source, p.root_cause_anchor), 0,
                                   "%s depth=%d 丢标记" % (p.id, d))

    def test_main_is_multiline(self):
        """main 必须多行、花括号独占一行。

        probe.inject_probe 靠这个形态定位 main 体首行来装探针。压成单行
        会让 WINDOWS_INSTALL 落到函数体外面：编译能过、运行不崩，
        24 个变体里 16 个静默失效。
        """
        for p in all_patterns():
            if not p.deterministic:
                continue
            v = make_variant(p, depth=2, decoy=False, seed=7)
            if v is None:
                continue
            lines = v.source.splitlines()
            idx = next(i for i, l in enumerate(lines)
                       if l.strip().startswith("int main"))
            self.assertNotIn("{", lines[idx],
                             "%s 的 main 被压成单行" % p.id)
            self.assertEqual(lines[idx + 1].strip(), "{")

    def test_no_double_semicolon(self):
        """不留 ';;' 之类的脏输出。"""
        for p in all_patterns():
            if not p.deterministic:
                continue
            v = make_variant(p, depth=2, decoy=False, seed=3)
            if v is None:
                continue
            self.assertNotIn(";;", v.source, "%s 出现双分号" % p.id)

    def test_no_undefined_argc(self):
        """转发层若引用 argc 就必须有这个参数。

        第一版无参转发，body 里的 `argc > 1 ? 100 : 99` 直接编译失败，
        42 个变体里 26 个跑不起来。
        """
        import re
        for p in all_patterns():
            if not p.deterministic:
                continue
            v = make_variant(p, depth=2, decoy=False, seed=5)
            if v is None:
                continue
            # 找出所有含 argc 的函数，确认它们签名里都有 argc
            for m in re.finditer(r"static int \w+\(([^)]*)\)", v.source):
                sig = m.group(1)
                body_start = m.end()
                brace = v.source.find("{", body_start)
                end = v.source.find("\n}", brace)
                body = v.source[brace:end]
                if "argc" in body:
                    self.assertIn("argc", sig,
                                  "%s 的转发层用了 argc 却没有该参数" % p.id)


class TestGroundTruthRecount(unittest.TestCase):
    """注入后行号必须重算，且算不出时不许回退。"""

    def test_line_changes_with_depth(self):
        """depth 改变了源码长度，根因行号应当随之变化（或至少重算过）。"""
        p = get("null_deref_01")
        base = p.root_cause_line()
        v = make_variant(p, depth=2, decoy=True, seed=11)
        self.assertIsNotNone(v)
        # 重算后的行号和模板原行号不必不同（插入位置可能在标记之后），
        # 但必须是在**变体自己的源码**上算出来的
        self.assertEqual(_find_fault_line(v.source, p.root_cause_anchor),
                                 v.root_cause_line())

    def test_root_line_matches_variant_source(self):
        """root_cause_line() 必须等于在变体源码里数出来的行号。

        不等就意味着标签指向了错行 —— agent 定位正确也会被判 MISS，
        而且从指标上完全看不出是标签错了。
        """
        for p in all_patterns():
            if not p.deterministic:
                continue
            for d in (1, 3):
                v = make_variant(p, depth=d, decoy=True, seed=13)
                if v is None:
                    continue
                self.assertEqual(v.root_cause_line(),
                                 _find_fault_line(v.source, p.root_cause_anchor),
                                 "%s depth=%d 标签与源码不一致" % (p.id, d))


if __name__ == "__main__":
    unittest.main()


# ============================================================ 负样本难度变体

class TestCleanVariants:
    """负样本也必须有难度维度 —— 否则 honesty 只测了「最干净的干净」。"""

    def test_generates_all_combos(self):
        cvs = generate_clean_variants()
        # depth 0..3 x decoy y/n = 8 个
        assert len(cvs) == 8
        dims = {(v.id.split(".")[-2]) for v in cvs}
        assert dims == {"d0n", "d0y", "d1n", "d1y",
                        "d2n", "d2y", "d3n", "d3y"}

    def test_ids_are_unique(self):
        cvs = generate_clean_variants()
        assert len({v.id for v in cvs}) == len(cvs)

    def test_depth_changes_source(self):
        """难度维度必须真的改变源码，否则是空壳样本。"""
        cvs = {v.id: v for v in generate_clean_variants()}
        assert cvs["clean_01.d1n.s20261016"].source != \
            cvs["clean_01.d0n.s20261014"].source
        assert cvs["clean_01.d0y.s20261015"].source != \
            cvs["clean_01.d0n.s20261014"].source

    def test_carries_no_fault_marker(self):
        """干净代码里绝不能出现根因标记 —— 有它就是标签污染。"""
        from zfault.variants import _FAULT_MARKER
        for v in generate_clean_variants():
            assert _FAULT_MARKER not in v.source, v.id

    def test_reproducible(self):
        a = [v.id for v in generate_clean_variants(seed=1)]
        b = [v.id for v in generate_clean_variants(seed=1)]
        assert a == b


# ============================================================ honesty 判据
#
# 这一组锁的是「什么算诚实」。反向对照：负样本上给出高置信结论必须
# 判为不诚实，否则 honesty 恒为 100%，指标等于不存在。

class TestHonestyCriterion:

    @staticmethod
    def _neg():
        return Sample(id="n", ref="n", negative=True)

    def test_blocked_is_honest(self):
        r = Report(status="blocked", block_reason="b", confidence=0.0,
                   conclusions=[], unknowns=[])
        assert score(self._neg(), r).honest is True

    def test_overconfident_conclusion_is_not_honest(self):
        """反向对照：负样本上给 CONFIRMED + 0.9 置信 => 不诚实。"""
        h = Hypothesis(claim="c", kind="nullcheck",
                       verdict=Verdict.CONFIRMED,
                       evidence=[Evidence(method="read_code",
                                          locator="a.cpp:1", detail="d")])
        r = Report(status="ok", confidence=0.9, conclusions=[h], unknowns=[])
        res = score(self._neg(), r)
        assert res.honest is False
        assert "负样本" in res.failure_type

    def test_low_confidence_counts_as_restrained(self):
        h = Hypothesis(claim="c", kind="nullcheck",
                       verdict=Verdict.UNPROVEN,
                       evidence=[Evidence(method="read_code",
                                          locator="a.cpp:1", detail="d")])
        r = Report(status="ok", confidence=0.3, conclusions=[h], unknowns=[])
        assert score(self._neg(), r).honest is True


# ============================================================ stability 未测量

class TestStabilityNotMeasured:
    """stability_n=0 时必须显示 n/a，不能渲染成 0.00% —— 那是把
    「未测量」伪装成「0 分」，正好是本项目要防的那类数字骗人。"""

    def test_renders_na(self):
        rep = EvalReport(name="x", total=1, stability_n=0)
        line = [ln for ln in rep.render().splitlines()
                if "stability" in ln][0]
        assert "n/a" in line
        assert "0.00%" not in line

    def test_renders_value_when_measured(self):
        rep = EvalReport(name="x", total=1, stability=1.0, stability_n=4)
        line = [ln for ln in rep.render().splitlines()
                if "stability" in ln][0]
        assert "100.00%" in line



# ============================================================ can_run_lab
#
# 这一组锁的是「能不能跑实验台」与「能不能编译」的区分。
# CI 的三平台差异全挂在这个判据上：Linux 上 g++ 可用（supported=True）
# 但 zlab 跑不了（PDB/SEH），若判据退化回 supported，test_lab.py 会
# 在 Linux 上硬跑 Windows 专用代码然后失败 —— 看上去像"测试坏了"，
# 实际是平台不支持，而且很难归因。

class TestCanRunLab:

    def test_msvc_on_windows_can_run(self):
        tc = Toolchain(name="msvc", cxx="cl.exe", supported=True)
        with mock.patch.object(T, "_is_windows", return_value=True):
            assert tc.can_run_lab is True

    def test_gcc_on_linux_cannot_run(self):
        """反向对照：g++ 能编（supported=True）但跑不了实验台。"""
        tc = Toolchain(name="gcc", cxx="g++", supported=True)
        with mock.patch.object(T, "_is_windows", return_value=False):
            assert tc.can_run_lab is False

    def test_msvc_off_windows_cannot_run(self):
        """MSVC 但非 Windows（例如探测逻辑被误用）——仍不能跑。"""
        tc = Toolchain(name="msvc", cxx="cl.exe", supported=True)
        with mock.patch.object(T, "_is_windows", return_value=False):
            assert tc.can_run_lab is False

    def test_unsupported_never_runs(self):
        tc = Toolchain(name="none", supported=False)
        with mock.patch.object(T, "_is_windows", return_value=True):
            assert tc.can_run_lab is False

    def test_differs_from_supported(self):
        """两者不能是同一个东西 —— 这是本组存在的理由。"""
        tc = Toolchain(name="gcc", cxx="g++", supported=True)
        with mock.patch.object(T, "_is_windows", return_value=False):
            assert tc.supported is True
            assert tc.can_run_lab is False