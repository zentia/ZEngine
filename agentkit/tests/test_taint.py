"""
回归测试：「检测点 -> 引入点」的追溯能力。

这一组测试守护的是本工具最有价值的能力跃迁：

    栈只能回答"在哪崩的"，而根因定位要回答"为什么崩"。

oob_write 崩在 memset（89 行），根因是上一行的 size_t 下溢（88 行）；
double_free 崩在堆管理器，根因是同函数体内对同一指针的第二次释放。
两者都不能靠栈得到，必须做语义追溯。

同时也守护实现这些追溯所依赖的**函数体界定**——
它一旦把 if/while/switch 误判成函数，所有依赖它的规则都会静默出错。
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.types import SourceLocation, Verdict
from zagent.verify import (DoubleFreeRule, HeuristicReasoner, RuleVerifier,
                           UnsignedUnderflowRule, _function_body)
from zfault.patterns import get


# ---------------------------------------------------------------- 桩件

class _MemCodeIndex:
    """内存代码索引桩。够用即可——这里测的是规则逻辑，不是 IO。"""

    def __init__(self, text: str, path: str = "f.cpp") -> None:
        self.text = text
        self.path = Path(path)
        self.lines = text.splitlines()

    def read_lines(self, path) -> list[str]:
        return self.lines

    def read_context(self, loc: SourceLocation, radius: int = 0) -> str:
        lo = max(0, loc.line - 1 - radius)
        hi = min(len(self.lines), loc.line + radius)
        return "\n".join(f"{i+1}: {self.lines[i]}" for i in range(lo, hi))

    def grep(self, pattern: str, limit: int = 200):
        import re
        rx = re.compile(pattern)
        return [(self.path, i, l)
                for i, l in enumerate(self.lines, 1) if rx.search(l)][:limit]


OOB_SRC = """\
void CopyRegion(char* dst, size_t dstCap, int idx) {
    Region r = ReadRegion(idx);
    size_t len = r.end - r.start;
    if (len > dstCap) {
        return;
    }
    std::memset(dst, 0xAB, len);
}

void CopyRegionUnchecked(char* dst, int idx) {
    Region r = ReadRegion(idx);
    size_t len = r.end - r.start;
    std::memset(dst, 0xAB, len);
}
"""

DF_SRC = """\
int Process(int mode) {
    char* buf = (char*)std::malloc(64);
    std::printf("%s\\n", buf);
    std::free(buf);
    if (mode == 2) {
        std::free(buf);
    }
    return 0;
}
"""


# ============================================================ 函数体界定

class TestFunctionBody(unittest.TestCase):
    """
    函数体界定是所有语义规则的公共地基。

    这里出错是静默的：规则照常返回结果，只是范围错了，
    于是"有保护"被判成"没保护"、"重复释放"被判成"不存在"。
    """

    def test_if_block_is_not_a_function(self):
        """`if (...) {` 不得被当成函数定义起点。"""
        code = _MemCodeIndex(DF_SRC)
        # 第 6 行在 if 块内部；若界定错误，起点会变成 if 那一行
        body = _function_body(code, SourceLocation(path=code.path, line=6))
        self.assertIsNotNone(body)
        joined = "\n".join(body)
        # 必须包含 if 块**之前**的第一次释放
        self.assertIn("std::free(buf)", joined)
        self.assertIn("int Process(int mode)", joined)

    def test_while_and_switch_are_not_functions(self):
        """while / switch 同样不是函数名。"""
        src = ('void F(int v) {\n'
               '    while (v) {\n'
               '        step();\n'
               '    }\n'
               '    switch (v) {\n'
               '        case 1:\n'
               '            break;\n'
               '    }\n'
               '}\n')
        code = _MemCodeIndex(src)
        for line in (3, 7):          # while 体内 / switch 体内
            body = _function_body(code, SourceLocation(path=code.path, line=line))
            self.assertIsNotNone(body, f"第 {line} 行未能界定函数体")
            self.assertIn("void F(int v)", "\n".join(body))


# ============================================================ 无符号下溢追溯

class TestUnsignedUnderflow(unittest.TestCase):
    """崩溃在 memset，根因在上一行的无符号减法。"""

    def test_confirms_underflow_and_points_to_assignment(self):
        code = _MemCodeIndex(OOB_SRC)
        # OOB_SRC 行号：10=函数定义 11=ReadRegion 12=size_t len=... 13=memset
        loc = SourceLocation(path=code.path, line=13)   # memset 所在行
        rule = UnsignedUnderflowRule(loc, "len")
        h = rule.verify(rule.to_hypothesis(), code)

        self.assertEqual(h.verdict, Verdict.CONFIRMED)
        # 证据必须指向赋值行（第 12 行），而不是崩溃行（第 13 行）
        self.assertIn(":12", h.evidence[0].locator)

    def test_refutes_when_size_is_constant(self):
        """常量长度不存在下溢，必须推翻而不是稀里糊涂证实。"""
        src = ('void F(char* dst) {\n'
               '    size_t len = 64;\n'
               '    std::memset(dst, 0, len);\n'
               '}\n')
        code = _MemCodeIndex(src)
        loc = SourceLocation(path=code.path, line=3)
        h = UnsignedUnderflowRule(loc, "len").verify(
            UnsignedUnderflowRule(loc, "len").to_hypothesis(), code)
        self.assertEqual(h.verdict, Verdict.REFUTED)

    def test_window_finds_write_call_after_return_address(self):
        """
        返回地址常落在调用行的**下一行**（实测 memset 在 89 行、
        定位落在 90 行）。只精确匹配崩溃行会一无所获。
        """
        code = _MemCodeIndex(OOB_SRC)
        # memset 在第 13 行，故意指向它**之后**的 14 行（`}`）
        found = HeuristicReasoner(code)._parse_write_call(
            SourceLocation(path=code.path, line=14))
        self.assertIsNotNone(found, "窗口内未找到写操作")
        func, size_arg = found
        self.assertEqual(func, "memset")
        self.assertEqual(size_arg, "len")


# ============================================================ 重复释放

class TestDoubleFree(unittest.TestCase):
    """同函数体内对同一指针的第二次释放。"""

    def test_points_to_second_free(self):
        code = _MemCodeIndex(DF_SRC)
        loc = SourceLocation(path=code.path, line=8)   # return 0;（定位常落在这一行）
        h = DoubleFreeRule(loc).verify(DoubleFreeRule(loc).to_hypothesis(), code)

        self.assertEqual(h.verdict, Verdict.CONFIRMED)
        # 第 6 行才是第二次释放（第 4 行是第一次）
        self.assertIn(":6", h.evidence[0].locator)

    def test_refutes_when_freed_once(self):
        src = ('int F() {\n'
               '    char* b = (char*)std::malloc(8);\n'
               '    std::free(b);\n'
               '    return 0;\n'
               '}\n')
        code = _MemCodeIndex(src)
        loc = SourceLocation(path=code.path, line=3)
        h = DoubleFreeRule(loc).verify(DoubleFreeRule(loc).to_hypothesis(), code)
        self.assertEqual(h.verdict, Verdict.REFUTED)


# ============================================================ 规则注册

class TestRuleRegistry(unittest.TestCase):
    """新规则必须注册进 RuleVerifier，否则假设永远停在 UNPROVEN。"""

    def test_new_kinds_registered(self):
        kinds = RuleVerifier().kinds()
        self.assertIn("unsigned_underflow", kinds)
        self.assertIn("double_free", kinds)


# ============================================================ 标签正确性

class TestGroundTruthAnchor(unittest.TestCase):
    """
    ground truth 必须指向**真实执行**的路径。

    oob_write 源码里有两个同样的下溢标记，而 main 只调用
    CopyRegionUnchecked。若取第一个匹配，标签会落在从不执行的
    死代码里 —— agent 定位对了也会被判错。
    """

    def test_oob_anchor_selects_executed_path(self):
        p = get("oob_write_01")
        self.assertNotEqual(p.root_cause_anchor, "", "oob_write 必须指定 anchor")

        idx_exec = p.source.find("CopyRegionUnchecked")
        idx_dead = p.source.find("void CopyRegion(")
        line_exec = p.source[:idx_exec].count("\n") + 1
        self.assertGreater(p.root_cause_line(), line_exec)
        self.assertLess(idx_dead, idx_exec)   # 死代码在前

    def test_other_patterns_have_no_anchor(self):
        """anchor 只能落在确实需要区分的模式上。"""
        for pid in ("null_deref_01", "double_free_01", "stack_overflow_01"):
            self.assertEqual(get(pid).root_cause_anchor, "", f"{pid} 不该有 anchor")


if __name__ == "__main__":
    unittest.main(verbosity=2)
