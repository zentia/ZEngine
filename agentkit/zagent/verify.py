"""
假设验证器与验证规则。

这是整套东西里唯一「换不掉」的部分——它承载领域知识。
骨架里只实现两类最通用的规则，用来跑通闭环；真实价值在于后续往里加规则。

设计原则（来自真实崩溃分析的教训）：
1. 每条结论必须挂可回溯证据（文件:行号 / 命令 / 计算过程）
2. 不允许「可能存在 X」这种不可证伪的表述
3. 被推翻的假设保留在台账里 —— 纠偏过程本身就是可审计资产
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Iterable

from .protocols import CodeIndex
from .types import Evidence, Facts, Hypothesis, SourceLocation, Verdict


# ============================================================ 规则实现

class CallChainRule:
    """
    调用链规则：验证 A 是否真的会调到 B。

    教训来源：真实分析里最容易犯的错就是「凭函数名猜调用关系」。
    函数名看起来像，不代表调用链存在——必须 grep 到调用点，并读函数体确认分支条件。
    """

    kind = "callchain"

    def __init__(self, caller: str, callee: str) -> None:
        self.caller = caller
        self.callee = callee

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=f"{self.caller} 会调用到 {self.callee}",
            kind=self.kind,
            data={"caller": self.caller, "callee": self.callee},
        )

    @classmethod
    def from_data(cls, data: dict) -> "CallChainRule | None":
        caller, callee = data.get("caller"), data.get("callee")
        if not caller or not callee:
            return None
        return cls(caller, callee)

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        # 1) 找到 callee 的定义点
        defs = code.grep(rf"\b{re.escape(self.callee)}\s*\(")
        if not defs:
            return h.refute(Evidence(
                method="grep",
                locator=f"grep '{self.callee}('",
                detail="未找到定义或调用点",
            ))

        # 2) 判断是否存在位于 caller 函数体内的调用
        call_sites = [
            (p, ln) for p, ln, _ in defs
            if _in_function(code, p, ln, self.caller)
        ]

        if call_sites:
            p, ln = call_sites[0]
            return h.confirm(Evidence(
                method="read_code",
                locator=f"{p}:{ln}",
                detail=f"在 {self.caller} 函数体内找到 {self.callee} 调用点",
            ))

        # 3) 有定义但不在 caller 体内 —— 兼容但未锁定，不是推翻
        p, ln, text = defs[0]
        h.evidence.append(Evidence(
            method="grep",
            locator=f"{p}:{ln}",
            detail=f"找到 {self.callee} 但未确认其位于 {self.caller} 调用路径上",
        ))
        h.verdict = Verdict.UNPROVEN
        h.note = "需要进一步确认调用路径（可能经中间层或回调）"
        return h


class NullCheckRule:
    """
    空指针规则：崩溃点变量在使用前是否有空判断保护。

    教训来源：崩溃行本身常常不是根因。一个 nullptr 解引用，
    真正的问题可能是「上游没有校验就传了下来」。
    """

    kind = "nullcheck"

    def __init__(self, location: SourceLocation | None = None, variable: str = "") -> None:
        self.location = location
        self.variable = variable

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=f"{self.variable} 在 {self.location} 处使用前缺少空判断",
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0,
                  "variable": self.variable},
        )

    @classmethod
    def from_data(cls, data: dict) -> "NullCheckRule | None":
        path, variable = data.get("path"), data.get("variable")
        if not path or not variable:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line), variable)

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        """
        只在「崩溃行所在函数体内」向上找保护。

        这一点很关键：早期版本用固定窗口（前 40 行）扫描，
        会把上一个函数（WriteNameSafe）里的保护错误地算到当前函数头上，
        从而把真正的漏洞判成「已有保护」。窗口扫描在边界处必然出错，
        必须按函数体界定范围。
        """
        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定崩溃行所在函数体，无法判定"
            return h

        use_line = self.location.line
        guard_re = re.compile(
            rf"(if\s*\(\s*!?{re.escape(self.variable)}\b"
            rf"|{re.escape(self.variable)}\s*!=\s*nullptr"
            rf"|{re.escape(self.variable)}\s*==\s*nullptr"
            rf"|{re.escape(self.variable)}\s*&&"
            rf"|assert\s*\(\s*{re.escape(self.variable)})"
        )

        before = [l for l in body if _line_no(l) is not None and _line_no(l) < use_line]
        guarded = [l for l in before if guard_re.search(_strip_no(l))]

        if guarded:
            ln = _line_no(guarded[-1])
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{ln}",
                detail=f"同函数体内发现空判断：{_strip_no(guarded[-1]).strip()[:70]}",
            ))

        return h.confirm(Evidence(
            method="read_code",
            locator=f"{self.location.path}:{use_line}",
            detail=(f"{self.location.path.name}:{use_line} 所在函数体内，"
                    f"使用前未发现对 {self.variable} 的空判断"),
        ))


class UnsignedUnderflowRule:
    """
    无符号下溢追溯规则：写操作的长度参数是否可能来自无符号减法。

    **它解决什么问题**

    崩溃点常常不是引入点。oob_write 里崩溃发生在
        std::memset(dst, 0xAB, len);      // 第 90 行，崩溃处
    但真正的缺陷是上一行
        size_t len = r.end - r.start;     // 第 88 行，下溢
    r.end(400) < r.start(1000)，size_t 无符号回绕成接近 SIZE_MAX 的巨大值。

    只看栈永远定位不到 88 行 —— 栈只能告诉你"在哪崩的"。
    要回到引入点必须做**同函数体内的数据流追溯**：
    崩溃行的长度参数是谁赋值的，赋值式是不是无符号减法。

    **为什么限定在函数体内**

    跨函数的数据流分析需要别名分析，做不对就会大量误报。
    限定在函数体内是保守但可靠的选择：
    绝大多数长度下溢就发生在调用点附近。
    追溯不到就保持 UNPROVEN —— 不猜是硬约束。
    """

    kind = "unsigned_underflow"

    def __init__(self, location: SourceLocation | None = None,
                 size_arg: str = "") -> None:
        self.location = location
        self.size_arg = size_arg

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=(f"{self.location} 的写操作长度参数 `{self.size_arg}` "
                   f"来自无符号减法，存在下溢风险"),
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0,
                  "size_arg": self.size_arg},
        )

    @classmethod
    def from_data(cls, data: dict) -> "UnsignedUnderflowRule | None":
        path = data.get("path")
        size_arg = data.get("size_arg")
        if not path or not size_arg:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line), size_arg)

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定崩溃行所在函数体，无法追溯"
            return h

        # 在函数体内，崩溃行之前，找该变量的赋值点
        assign_re = re.compile(
            rf"\b(?:size_t|unsigned|uint\d+_t|std::size_t)\s+"
            rf"{re.escape(self.size_arg)}\s*=\s*(.+?)\s*;")
        before = [l for l in body if _line_no(l) is not None
                  and _line_no(l) <= self.location.line]

        for line in reversed(before):
            m = assign_re.search(_strip_no(line))
            if not m:
                continue
            expr = m.group(1)
            ln = _line_no(line)

            # 无符号减法：a - b，且操作数不是字面量常量
            sub = re.search(r"(\w+(?:\.\w+)*)\s*-\s*(\w+(?:\.\w+)*)", expr)
            if not sub:
                # 有赋值但并非减法 —— 推翻"下溢"这个具体假设
                return h.refute(Evidence(
                    method="read_code",
                    locator=f"{self.location.path}:{ln}",
                    detail=(f"{self.size_arg} 的赋值式为 `{expr}`，"
                            f"非减法运算，不存在下溢"),
                ))
            if sub.group(1).isdigit() and sub.group(2).isdigit():
                return h.refute(Evidence(
                    method="read_code",
                    locator=f"{self.location.path}:{ln}",
                    detail=f"{self.size_arg} = {expr} 为常量运算，不会下溢",
                ))

            return h.confirm(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{ln}",
                detail=(f"{self.size_arg} = `{expr}` 为无符号减法："
                        f"若被减数小于减数将回绕成巨大值，"
                        f"导致 {self.location.line} 行的写操作越界"),
            ))

        h.verdict = Verdict.UNPROVEN
        h.note = (f"未在函数体内找到 {self.size_arg} 的无符号赋值点，"
                  f"可能在更外层（不做跨函数追溯，避免误报）")
        return h


class DoubleFreeRule:
    """
    重复释放规则：同函数体内同一指针是否被释放两次。

    **为什么需要它**

    栈能告诉你"崩在堆管理器里"（double_free 崩在 ntdll，
    crash_address 为 None，靠栈帧才回溯到 f.cpp 的 Process）。
    但"崩在 free 附近"不等于"找到了重复释放那一行"。

    实测 double_free：第二次 free 在第 76 行，而定位落在第 78 行 ——
    返回地址被归到了函数末尾的 `return 0;`。
    靠行号吸附去猜是不可靠的；可靠的判据是**语义**：
    同一个函数体内，同一个指针变量出现了两次释放，
    那第二次就是重复释放 —— 这是可证伪的。

    **为什么不跨函数**

    跨函数需要别名分析与路径敏感分析。做不对会大量误报
    （例如 a 分支释放、b 分支也释放，但二者互斥，并非重复释放）。
    限定在同一函数体内是保守但可靠的选择。
    """

    kind = "double_free"

    def __init__(self, location: SourceLocation | None = None) -> None:
        self.location = location

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=f"{self.location} 所在函数体内存在对同一指针的重复释放",
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0},
        )

    @classmethod
    def from_data(cls, data: dict) -> "DoubleFreeRule | None":
        path = data.get("path")
        if not path:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line))

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定崩溃行所在函数体，无法判定"
            return h

        # 必须同时认 ZLabFree：接管分配器之后，源码里的释放调用
        # 已经被替换成这个名字（注入改变了调用名，规则必须跟上）。
        # 只认 std::free 会让"重复释放"永远查不到 ——
        # 实测 double_free 因此被 REFUTED，定位停在返回地址那一行。
        free_re = re.compile(
            r"(?:(?:std::)?(?:free|ZLabFree)\s*\(\s*(\w+)\s*\)"
            r"|\bdelete\s*(?:\[\s*\])?\s*(\w+))")

        seen: dict[str, int] = {}
        dup: tuple[str, int] | None = None
        for line in body:
            m = free_re.search(_strip_no(line))
            if not m:
                continue
            var = m.group(1) or m.group(2)
            ln = _line_no(line)
            if ln is None:
                continue
            if var in seen:
                dup = (var, ln)          # 第二次释放：重复的那一处
                break
            seen[var] = ln

        if dup is None:
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{self.location.line}",
                detail="崩溃行所在函数体内未发现对同一指针的重复释放",
            ))

        var, ln = dup
        return h.confirm(Evidence(
            method="read_code",
            locator=f"{self.location.path}:{ln}",
            detail=(f"指针 `{var}` 在同一函数体内被释放两次，"
                    f"第二次释放在第 {ln} 行（重复释放点）"),
        ))


class LeakRule:
    """
    泄漏规则：分配之后是否存在"未释放就提前返回"的分支。

    **为什么需要它**

    泄漏检测能给出的是**分配点**（`ctx->a = ZLabAlloc(64)`），
    但根因不在这里 —— 分配本身没错，错的是有一条路径走到
    `return -1;` 却没有释放。实测两者差 2 行，
    而命中判定是严格的行相等（零容差），差一行就是 MISS。

    **为什么判据是"分配之后的非零 return"**

    靠行号吸附去猜不可靠（与 double_free 同理）。
    可靠的语义判据是：
      1. 从分配点向下，同一函数体内；
      2. 存在一条 `return <非 0>` —— 非零即失败早退，
         正常成功路径不会这么返回；
      3. 该 return 之前没有发生过释放。
    三者同时成立，那一行就是泄漏点。

    这是**可证伪**的：函数体里若没有非零 return（一路走到底正常释放），
    规则就 REFUTED，不会硬凑一个位置。

    **为什么只取第一个**

    多个早退分支都可能是泄漏点。但 ground truth 只标一个，
    取"分配之后最近的那条早退"最贴近「泄漏发生的第一现场」，
    也与"泄漏块是在这条 return 上被丢掉的"这一事实一致。
    """

    kind = "leak"

    def __init__(self, location: SourceLocation | None = None) -> None:
        self.location = location

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=f"{self.location} 分配之后存在未释放即提前返回的分支",
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0},
        )

    @classmethod
    def from_data(cls, data: dict) -> "LeakRule | None":
        path = data.get("path")
        if not path:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line))

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定分配点所在函数体，无法判定"
            return h

        start = self.location.line

        # 分配之后的早退：return 一个非零常量/表达式。
        # 只认非零 —— 成功的正常路径通常以 return 0 收尾，
        # 把它也算进来会把函数末尾误判成泄漏点。
        ret_re = re.compile(r"\breturn\s+([^;]+);")
        # 释放调用：与 DoubleFreeRule 一致，必须认接管后的名字
        free_re = re.compile(
            r"(?:(?:std::)?(?:free|ZLabFree)\s*\(\s*(\w+)\s*\)"
            r"|\bdelete\s*(?:\[\s*\])?\s*(\w+))")

        for line in body:
            ln = _line_no(line)
            if ln is None or ln <= start:
                continue
            code_part = _strip_no(line)

            # 先遇到释放：这条路径上没有泄漏
            if free_re.search(code_part):
                return h.refute(Evidence(
                    method="read_code",
                    locator=f"{self.location.path}:{ln}",
                    detail=(f"分配点之后第 {ln} 行即发生释放，"
                            f"不存在未释放就返回的分支"),
                ))

            m = ret_re.search(code_part)
            if not m:
                continue
            expr = m.group(1).strip()
            # 非零返回 = 失败早退。排除 return 0 / return rc 这类成功路径。
            if expr in ("0", "rc", "EXIT_SUCCESS"):
                continue
            return h.confirm(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{ln}",
                detail=(f"分配点（第 {start} 行）之后第 {ln} 行以 "
                        f"`return {expr};` 提前返回，此前未见释放 —— 泄漏点"),
            ))

        return h.refute(Evidence(
            method="read_code",
            locator=f"{self.location.path}:{start}",
            detail="分配点之后未发现提前返回的分支",
        ))


class RecursionRule:
    """
    无限递归规则：该行的自递归是否**没有编译期常量封顶**。

    **为什么需要它**

    实测 stack_overflow_01 定位到了 446 行、判定 HIT，
    但它的报告里**零结论、置信度 0.0** ——
    和 race_01 补规则之前是同一类虚命中：
    符号化恰好指对了行号，验证层完全没参与。
    这种"命中"换个样本就会露馅。

    **判据（两条同时成立才 CONFIRMED）**

    1. 该行调用了**所在函数自身**（直接或间接自递归）；
    2. 递归的终止条件**依赖函数参数**，而不是编译期常量上限。

    第 2 条是关键。正常的递归长这样：
        if (depth >= kMaxDepth) return 0;   // 常量封顶，永远会停
    而 stack_overflow_01 长这样：
        if (limit >= 0 && depth >= limit) return 0;   // 依赖参数 limit
        return Evaluate(depth + 1, limit) + 1;
    `limit` 由 main 传入（实测传 -1），条件可能永不满足 ——
    这就是"递归本身没错，错的是终止条件不由自己控制"。

    **可证伪**：若有 `kMaxDepth` 之类的常量上限，判据 2 不成立，REFUTED。
    单线程的非递归调用，判据 1 不成立，同样 REFUTED。
    """

    kind = "recursion"

    # 终止/返回条件：if (...) 或 return 后的条件表达式
    _COND_RE = re.compile(r"\bif\s*\(([^)]*)\)")

    @staticmethod
    def _strip_comment(text: str) -> str:
        """
        去掉行注释与块注释残留。

        **这条是被实测逼出来的。** stack_overflow_01 的注释里写着
        「最初写成 `if (depth > limit) return 0;`」——
        那是一段**已经被弃用的伪代码**，却长得和真代码一模一样。
        不剥注释的话，规则会把注释里的条件当成真实终止条件，
        不仅证据指错行，连判据本身都建立在一段说明文字上。
        """
        # 行注释：注意不要误伤字符串里的 //，这里够用且不追求完美
        i = text.find("//")
        if i >= 0:
            text = text[:i]
        text = re.sub(r"/\*.*?\*/", " ", text)
        return text

    def __init__(self, location: SourceLocation | None = None,
                 func_name: str = "") -> None:
        self.location = location
        self.func_name = func_name

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=(f"{self.func_name or '(函数)'} 在 {self.location} 处"
                   f"自递归且终止条件依赖参数"),
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0,
                  "func_name": self.func_name},
        )

    @classmethod
    def from_data(cls, data: dict) -> "RecursionRule | None":
        path = data.get("path")
        if not path:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line),
                   data.get("func_name", "") or "")

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        if code is None or self.location is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无代码索引或定位缺失，无法判定"
            return h

        try:
            lines = code.read_lines(self.location.path)
        except Exception:
            lines = []
        if not lines:
            h.verdict = Verdict.UNPROVEN
            h.note = "读不到源码，无法判定"
            return h

        idx = self.location.line - 1
        if idx < 0 or idx >= len(lines):
            h.verdict = Verdict.UNPROVEN
            h.note = "定位行超出源码范围"
            return h
        text = lines[idx]

        func_name = (self.func_name
                     or _enclosing_function_name(lines, self.location.line)
                     or "")

        # ---- 判据 1：该行调用了所在函数自身 ----
        if not func_name or not re.search(
                rf"\b{re.escape(func_name)}\s*\(", text):
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{self.location.line}",
                detail=(f"该行未调用 {func_name or '所在函数'} 自身，"
                        f"不是自递归：{text.strip()[:60]}"),
            ))

        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定所在函数体，无法判定终止条件"
            return h

        # ---- 提取参数名（终止条件是否受外部控制，看的就是它）----
        params = _function_params(lines, self.location.line, func_name)

        # ---- 判据 2：终止条件依赖参数，而非编译期常量 ----
        # 先看有没有"完全不含参数"的常量封顶（那种递归一定会停）
        for bl in body:
            ln = _line_no(bl)
            if ln is None or ln == self.location.line:
                continue
            code_part = self._strip_comment(_strip_no(bl))
            if not code_part.strip():
                continue
            for cond in self._COND_RE.findall(code_part):
                if not any(re.search(rf"\b{re.escape(p)}\b", cond)
                           for p in params):
                    # 条件里一个参数都没有 —— 纯常量上限，递归会终止
                    if re.search(r"\d", cond):
                        return h.refute(Evidence(
                            method="read_code",
                            locator=f"{self.location.path}:{ln}",
                            detail=(f"第 {ln} 行存在不依赖参数的常量终止条件 "
                                    f"`{cond.strip()[:40]}`，递归有上限"),
                        ))

        # 存在依赖参数的终止条件 —— 参数由外部传入，可能永不满足
        for bl in body:
            ln = _line_no(bl)
            if ln is None or ln == self.location.line:
                continue
            code_part = self._strip_comment(_strip_no(bl))
            if not code_part.strip():
                continue
            for cond in self._COND_RE.findall(code_part):
                hit = [p for p in params
                       if re.search(rf"\b{re.escape(p)}\b", cond)]
                if hit:
                    return h.confirm(Evidence(
                        method="read_code",
                        locator=f"{self.location.path}:{self.location.line}",
                        detail=(f"{func_name} 自递归，第 {ln} 行的终止条件 "
                                f"`{cond.strip()[:40]}` 依赖参数 "
                                f"{'/'.join(hit)} —— 参数由外部传入时"
                                f"可能永不满足，递归无上限"),
                    ))

        return h.refute(Evidence(
            method="read_code",
            locator=f"{self.location.path}:{self.location.line}",
            detail="函数体内未发现任何终止条件，无法判定为无限递归",
        ))


class OverflowRule:
    """
    整数溢出规则：分配大小的乘法是否在**窄宽度**上溢出。

    **为什么需要它**

    崩溃发生在循环写入（`memset(&table[i], ...)`），
    但根因是 `count * sizeof(T)` 的乘法 —— 两者相差好几行。
    栈只能告诉你"在哪崩的"，而这里连崩溃点都不是引入点。

    本规则的输入是**分配点的返回地址**（ZLAB_HIT_ALLOC_RA），
    它由探测器在"写穿活跃块"时输出，符号化后落在分配函数体内。
    没有这条运行时证据，整数溢出在静态上几乎无法与正常乘法区分 ——
    这正是 overflow_01 长期被判为负样本（走降级路径）的原因。

    **判据（同时成立才 CONFIRMED）**

    1. 分配点所在函数体内存在**整型乘法**，且乘数之一是 `sizeof(...)`。
       这是 `count * sizeof(T)` 的经典形态。
    2. 乘积赋给**比 size_t 窄**的整型（如 unsigned int / int）。
       宽度是关键：`(size_t)count * sizeof(T)` 先提升再乘，
       count 是 int（上限 2^31）时乘积上限约 2^39，
       在 64 位 size_t 下**永远不可能溢出**。
       只有"窄宽度乘法先溢出、再赋给宽类型"才是真漏洞。
    3. 该乘积随后被用作**分配大小**（传给 malloc / ZLabAlloc）。

    **可证伪性**

    普通的一次分配（如 leak 的 `malloc(64)`）没有乘法，第 1 条不成立，
    会被 REFUTED。oob_write 的长度来自减法而非乘法，同样不成立。
    不会污染其它样本。
    """

    kind = "overflow"

    # 整型乘法且含 sizeof。
    # **不能要求 sizeof 紧邻 `*`**：实际写法常带强制转换
    # `(unsigned int)count * (unsigned int)sizeof(BigEntry)`，
    # 强制转换插在中间会让"紧邻"型正则直接漏掉真漏洞。
    # 所以判据放宽为：同一行里既有 `*` 也有 sizeof(...)。
    _MUL_RE = re.compile(r"\*.*sizeof\s*\(|sizeof\s*\([^)]*\).*\*")

    # 窄宽度整型：明显比 size_t 窄。size_t / uint64_t 不算。
    _NARROW_RE = re.compile(
        r"\b(?:unsigned\s+int|unsigned\s+short|unsigned\s+char|int|short|char"
        r"|uint8_t|uint16_t|uint32_t|int8_t|int16_t|int32_t)\b")

    # 宽整型：出现即为"先提升再乘"，溢出不可能发生
    _WIDE_RE = re.compile(r"\b(?:size_t|std::size_t|uint64_t|int64_t|"
                          r"unsigned\s+long\s+long|long\s+long)\b")

    # 分配调用
    _ALLOC_RE = re.compile(r"\b(?:std::)?(?:malloc|calloc|realloc|ZLabAlloc)\s*\(")

    def __init__(self, location: "SourceLocation | None" = None) -> None:
        self.location = location

    def to_hypothesis(self) -> "Hypothesis":
        return Hypothesis(
            claim=(f"{self.location} 附近的分配大小由窄宽度乘法得出，"
                   f"存在整数溢出风险"),
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0},
        )

    @classmethod
    def from_data(cls, data: dict) -> "OverflowRule | None":
        path = data.get("path")
        if not path:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line))

    def verify(self, h: "Hypothesis", code: "CodeIndex") -> "Hypothesis":
        if code is None or self.location is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无代码索引或定位缺失，无法判定"
            return h

        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定分配点所在函数体，无法追溯"
            return h

        # 在函数体内找"窄宽度整型 + 乘法(sizeof)"的赋值点
        narrow_assign = None
        mul_line = None
        for line in body:
            ln = _line_no(line)
            if ln is None:
                continue
            text = _strip_no(line)
            if not self._MUL_RE.search(text):
                continue
            mul_line = ln
            # 声明类型决定乘法宽度：必须是窄整型，且不能是宽整型
            if self._NARROW_RE.search(text) and not self._WIDE_RE.search(text):
                narrow_assign = (ln, text.strip())
                break

        if narrow_assign is None:
            detail = ("函数体内未发现窄宽度整型乘法"
                      + (f"（第 {mul_line} 行有乘法，但操作数宽度为 size_t，"
                         f"先提升再乘，在 64 位下不会溢出）" if mul_line else ""))
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{self.location.line}",
                detail=detail,
            ))

        ln, expr = narrow_assign

        # 乘积必须真的被用作分配大小，否则只是一次普通算术
        alloc_line = None
        for line in body:
            n2 = _line_no(line)
            if n2 is None:
                continue
            if self._ALLOC_RE.search(_strip_no(line)):
                alloc_line = n2
                break

        if alloc_line is None:
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{ln}",
                detail=f"`{expr}` 未作为分配大小使用，不构成溢出分配",
            ))

        return h.confirm(Evidence(
            method="read_code",
            locator=f"{self.location.path}:{ln}",
            detail=(f"`{expr}` 为窄宽度整型乘法（第 {ln} 行），"
                    f"结果回绕后传给第 {alloc_line} 行的分配调用，"
                    f"分配到的内存远小于后续写入量 —— "
                    f"崩溃在循环写入，根因在此处"),
        ))


class RaceRule:
    """
    数据竞争规则：该行是否对共享变量做了**无同步的读改写**。

    **为什么需要它**

    竞争检测能给出冲突现场的指令地址（guard 页捕获到第二个线程访问），
    符号化后落在 `g_counter++` 那一行。但**地址对上行号不等于证实** ——
    实测 race_01 的报告里没有任何 CONFIRMED 结论，只有一条
    "栈帧过少"的 unproven，置信度只有 0.15。
    行号之所以等于期望值，只是因为运行时证据恰好指对了地方，
    验证层根本没有参与。那样的"命中"是虚的：换个样本就会露馅。

    **判据（三条同时成立才 CONFIRMED，缺一条即 REFUTED）**

    1. 该行对一个变量做**读改写**：`++` / `--` / `+=` / `x = x + ...`。
       单次读或单纯的赋写不构成竞争 —— 竞争的成因正是
       "读-改-写"三步之间可以插入别的线程。
    2. 该变量是**文件作用域的共享变量**，且所在函数被
       **多线程入口**触及（作为 std::thread 等传入）。
       栈上局部变量跨不了线程，不构成共享。
    3. 访问路径上**没有同步原语**（mutex / lock_guard / atomic）。

    这是**可证伪**的：oob_write 之类单线程越界写，第 2 条就不成立，
    会被正常推翻，不会污染其它样本的定位。

    **为什么不做 happens-before**

    完整的竞争检测（TSan 那套）要维护向量时钟、拦截每一次同步操作。
    这里做不到，也不需要：判据 1+2+3 已足以区分
    "无保护的共享读改写"与"正常代码"，而**定位**只需要这一个判断 ——
    不需要穷举所有竞争对。
    """

    kind = "race"

    # 读改写：自增/自减/复合赋值/自身参与运算后回写。
    # 只认这四种形态，不认 `x = y;` —— 单纯的赋写不构成读改写。
    _RMW_RE = re.compile(
        r"\b(\w+)\s*(\+\+|--|\+=|-=|\*=|/=)"
        r"|\b(\w+)\s*=\s*\3\s*[-+*/]")

    # 同步原语。出现在同一函数体内即视为有保护。
    _SYNC_RE = re.compile(
        r"\b(std::)?(mutex|recursive_mutex|shared_mutex|atomic)\b"
        r"|\block_guard\b|\bunique_lock\b|\bscoped_lock\b"
        r"|\.lock\s*\(|\.unlock\s*\(|\bfetch_add\b|\bcompare_exchange"
        r"|\bInterlocked\w*\s*\(")

    def __init__(self, location: SourceLocation | None = None,
                 variable: str = "") -> None:
        self.location = location
        self.variable = variable

    def to_hypothesis(self) -> Hypothesis:
        return Hypothesis(
            claim=(f"{self.variable or '(变量)'} 在 {self.location} 处"
                   f"被多线程无同步地读改写"),
            kind=self.kind,
            data={"path": str(self.location.path) if self.location else "",
                  "line": self.location.line if self.location else 0,
                  "variable": self.variable},
        )

    @classmethod
    def from_data(cls, data: dict) -> "RaceRule | None":
        path = data.get("path")
        if not path:
            return None
        try:
            line = int(data.get("line", 0))
        except (TypeError, ValueError):
            return None
        return cls(SourceLocation(path=Path(path), line=line),
                   data.get("variable", "") or "")

    def verify(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        if code is None or self.location is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无代码索引或定位缺失，无法判定"
            return h

        try:
            lines = code.read_lines(self.location.path)
        except Exception:
            lines = []
        if not lines:
            h.verdict = Verdict.UNPROVEN
            h.note = "读不到源码，无法判定"
            return h

        idx = self.location.line - 1
        if idx < 0 or idx >= len(lines):
            h.verdict = Verdict.UNPROVEN
            h.note = "定位行超出源码范围"
            return h
        text = lines[idx]

        # ---- 判据 1：该行是读改写，并取出被改写的变量 ----
        m = self._RMW_RE.search(text)
        if not m:
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{self.location.line}",
                detail=(f"该行不是读改写（无 ++/--/复合赋值/自身回写），"
                        f"不构成竞争：{text.strip()[:60]}"),
            ))
        var = m.group(1) or m.group(3) or (self.variable or "")

        # ---- 判据 2a：变量是文件作用域的共享变量 ----
        # 声明行必须在 depth==0（不在任何函数体内），否则是局部变量。
        shared = False
        depth = 0
        decl_re = re.compile(
            rf"\b(?:int|long|short|unsigned|volatile|std::atomic)?"
            rf"[\w:<>\s\*&]*\b{re.escape(var)}\s*(?:=|;)")
        for ln in lines:
            if depth == 0 and decl_re.search(ln):
                shared = True
                break
            depth += ln.count("{") - ln.count("}")
        if not shared:
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{self.location.line}",
                detail=f"{var} 不是文件作用域变量，跨线程不共享，不构成竞争",
            ))

        # ---- 判据 2b：所在函数被多线程入口触及 ----
        body = _function_body(code, self.location)
        if body is None:
            h.verdict = Verdict.UNPROVEN
            h.note = "无法界定所在函数体，无法判定是否被多线程调用"
            return h
        func_name = _enclosing_function_name(lines, self.location.line)
        threaded = bool(func_name) and _is_thread_entry(lines, func_name)
        if not threaded:
            return h.refute(Evidence(
                method="read_code",
                locator=f"{self.location.path}:{self.location.line}",
                detail=(f"{func_name or '所在函数'} 未作为线程入口被启动，"
                        f"不存在并发访问"),
            ))

        # ---- 判据 3：访问路径上没有同步原语 ----
        for bl in body:
            if self._SYNC_RE.search(_strip_no(bl)):
                return h.refute(Evidence(
                    method="read_code",
                    locator=f"{self.location.path}:{_line_no(bl)}",
                    detail=(f"同一函数体内第 {_line_no(bl)} 行存在同步原语，"
                            f"访问受保护，不构成竞争"),
                ))

        return h.confirm(Evidence(
            method="read_code",
            locator=f"{self.location.path}:{self.location.line}",
            detail=(f"{var} 是共享变量，所在函数 {func_name} 被多线程启动，"
                    f"该行做读改写的全过程中未见同步原语 —— 竞争点"),
        ))


# ============================================================ 组合验证器

class RuleVerifier:
    """
    把假设按 kind 分发到对应规则实现。

    关键约束：没有对应规则的假设保持 UNPROVEN，绝不默认证实。
    「无法判断」是合法结论，「猜一个」不是。
    """

    def __init__(self) -> None:
        self._rules: dict[str, type] = {
            "callchain": CallChainRule,
            "nullcheck": NullCheckRule,
            "unsigned_underflow": UnsignedUnderflowRule,
            "double_free": DoubleFreeRule,
            "leak": LeakRule,
            "race": RaceRule,
            "recursion": RecursionRule,
            "overflow": OverflowRule,
        }

    def register(self, kind: str, rule_cls: type) -> None:
        self._rules[kind] = rule_cls

    def kinds(self) -> list[str]:
        return sorted(self._rules)

    def check(self, h: Hypothesis, code: CodeIndex) -> Hypothesis:
        rule_cls = self._rules.get(h.kind)
        if rule_cls is None:
            h.verdict = Verdict.UNPROVEN
            h.note = f"无 {h.kind} 类型的验证规则，无法判定"
            return h

        try:
            rule = rule_cls.from_data(h.data) if hasattr(rule_cls, "from_data") else None
        except (KeyError, TypeError):
            rule = None

        if rule is None:
            h.verdict = Verdict.UNPROVEN
            h.note = f"{h.kind} 规则缺少必要参数，无法判定"
            return h

        return rule.verify(h, code)


# ============================================================ 启发式假设生成

class HeuristicReasoner:
    """
    规则驱动的假设生成器（无 LLM 版本）。

    存在的意义：让流水线在没有模型的情况下也能跑，从而让 eval 有一个稳定基线。
    接 LLM 时替换成 LLMReasoner 即可，接口一致。

    注意：这里只产出可证伪的假设。推断不出变量的假设不产出——
    「无法验证」优于「猜一个然后假装验证了」。
    """

    def __init__(self, code: "CodeIndex | None" = None) -> None:
        self.code = code

    def _propose_underflow(self, facts, top) -> "Hypothesis | None":
        """
        写越界场景：从崩溃行的写操作回溯长度参数来源。

        这是「检测点 ≠ 引入点」最典型的情形，也是规则推理能超过
        纯栈分析的地方：栈只能告诉你在 memset 崩了，
        但真正的缺陷是传给它的 len 下溢了。
        """
        write = self._parse_write_call(top)
        if write is None:
            return None
        func, size_arg = write
        return UnsignedUnderflowRule(top, size_arg).to_hypothesis()

    def _parse_write_call(self, loc: SourceLocation) -> "tuple[str, str] | None":
        """
        在崩溃行附近解析写操作：(被调用函数名, 长度参数表达式)。

        **为什么不能只精确匹配崩溃行**

        实测 oob_write：memset 在第 89 行，但符号化定位到第 90 行。
        原因：崩溃地址是调用指令**之后**的返回地址，
        MSVC 在尾调用场景下会把它归到函数末尾的 `}` 上。
        只匹配崩溃行会一无所获，进而以为"这里没有写操作"。

        所以在一个很小的窗口（前 2 行 ~ 后 1 行）内找，
        且取**离崩溃行最近**的那个 —— 偏后优先，因为返回地址
        通常在调用行或紧随其后。窗口必须小：放宽到整个函数体
        会把无关的写操作也拉进来，产生错误追溯。

        只认最常见的批量写原语，认不出来返回 None。
        不做模糊匹配 —— 猜错参数位置会产生完全错误的追溯结论。
        """
        for delta in (0, 1, -1, -2):
            cand = SourceLocation(path=loc.path, line=loc.line + delta)
            ctx = self.code.read_context(cand, radius=0) if self.code else ""
            for line in ctx.splitlines():
                if _line_no(line) != cand.line:
                    continue
                text = _strip_no(line)
                m = re.search(
                    r"(?:std::)?(memset|memcpy|memmove|wmemset|memset_s)\s*\(", text)
                if not m:
                    continue
                args = _split_args(text[m.end():])
                # memset(dst, val, len) / memcpy(dst, src, len)：长度在最后
                if len(args) < 3:
                    continue
                size_arg = args[-1].strip()
                # 长度必须是标识符或成员访问，字面量没有追溯价值
                if not re.fullmatch(r"\w+(?:\.\w+)*", size_arg):
                    continue
                return m.group(1), size_arg
        return None

    def propose(self, facts: Facts, locations: list[SourceLocation]) -> list[Hypothesis]:
        out: list[Hypothesis] = []
        code = self.code

        if not locations:
            return out

        top = locations[0]

        # 1) 顶层帧是否真属于项目代码 —— 很多崩溃栈顶是运行时/系统帧
        if facts.stack and not facts.stack[0].is_project_code:
            out.append(Hypothesis(
                claim="栈顶帧不属于项目代码，崩溃点可能只是检测点而非破坏发生点",
                kind="ownership",
            ))

        # 2) 异常类型驱动的猜测（带上规则参数，否则验证器无法重建规则）
        exc = (facts.exception_type or "").upper()
        if "ACCESS_VIOLATION" in exc or "SEGV" in exc:
            var = _guess_variable(code, top) if code is not None else None
            if var:
                out.append(NullCheckRule(top, var).to_hypothesis())
            else:
                out.append(Hypothesis(
                    claim="崩溃行未能识别出被解引用的变量，无法做空判断验证",
                    kind="nullcheck",
                ))
        elif "HEAP_CORRUPTION" in exc:
            out.append(Hypothesis(
                claim=f"{top} 是破坏被检测到的位置，真正的写越界发生在此之前",
                kind="ownership",
            ))

        # 2.6) 堆破坏类异常：查同函数体内的重复释放。
        #     崩在 ntdll 时栈只能指回项目函数，语义分析才能落到"第二次 free"。
        # 接管分配器之后，重复释放表现为**访问违例**
        # （块被置为 PAGE_NOACCESS，再访问必然 AV），
        # 不再是 CRT 的堆校验异常。只认 HEAP_* 会让这条规则永不触发 ——
        # 实测 double_free 因此一直拿不到"第二次释放"的位置。
        #
        # 放宽到 ACCESS_VIOLATION 是安全的：规则本身是可证伪的，
        # 函数体内没有重复释放就 REFUTED，不会动定位。
        # oob_write 同样是 AV，但它的函数体里只有一次分配、没有重复释放，
        # 会被正常推翻。
        if code is not None and ("HEAP_CORRUPTION" in exc
                                 or "HEAP_FAILURE" in exc
                                 or "STATUS_HEAP" in exc
                                 or "ACCESS_VIOLATION" in exc):
            out.append(DoubleFreeRule(top).to_hypothesis())

        # 2.4) 泄漏：顶层位置若落在**分配调用**上，说明这是泄漏块的
        #      分配点（只有泄漏定位才会把分配点当作顶层位置），
        #      进一步验证"分配之后是否有未释放就返回的分支"。
        #
        #      不依赖异常类型：泄漏根本不产生异常，
        #      用 exception_type 做门控会让这条规则永不触发。
        if code is not None:
            ctx = code.read_context(top, radius=0)
            for cl in ctx.splitlines():
                if _line_no(cl) != top.line:
                    continue
                if re.search(r"ZLabAlloc\s*\(", _strip_no(cl)):
                    out.append(LeakRule(top).to_hypothesis())
                break

        # 2.6) 竞争：顶层位置若落在**读改写**上，且文件里真的起了线程，
        #      进一步验证"该共享变量的访问是否被同步保护"。
        #
        #      同样**不依赖异常类型**：竞争根本不崩溃，也没有栈
        #      （实测 race_01 的 stack 为空），
        #      用 exception_type 或栈深度做门控会让这条规则永不触发。
        #
        #      门控用"文件里存在线程入口"而不是"include 了 <thread>"：
        #      后者太宽，单线程程序也可能 include 却从不启动。
        #      规则本身是可证伪的，不满足三条判据就 REFUTED，不会污染定位。
        if code is not None:
            ctx = code.read_context(top, radius=0)
            for cl in ctx.splitlines():
                if _line_no(cl) != top.line:
                    continue
                text = _strip_no(cl)
                if RaceRule._RMW_RE.search(text):
                    try:
                        src_lines = code.read_lines(top.path)
                    except Exception:
                        src_lines = []
                    fname = _enclosing_function_name(src_lines, top.line)
                    if fname and _is_thread_entry(src_lines, fname):
                        out.append(RaceRule(top).to_hypothesis())
                break

        # 2.7) 无限递归：栈溢出（STATUS_STACK_OVERFLOW）时，
        #      栈只能指回递归调用那一行，语义分析才能说明
        #      "为什么递归停不下来"。
        #
        #      门控同时认异常码与栈深：栈溢出有 34 帧、异常码明确，
        #      两者都能用；只靠栈深会把浅栈的其它样本也带进来，
        #      不过规则本身可证伪，误带入也会被 REFUTED。
        if code is not None and ("STACK_OVERFLOW" in exc
                                 or len(facts.stack) >= 8):
            out.append(RecursionRule(top).to_hypothesis())

        # 2.8) 整数溢出：分配点所在函数体内若存在**分配调用**，
        #      进一步验证大小的乘法是否发生在窄宽度上。
        #
        #      同样**不依赖异常类型**：崩溃类型是 ACCESS_VIOLATION，
        #      与空指针、越界写完全相同，靠它无法区分。
        #      真正的门控是运行时证据 —— 只有当探测器报告了
        #      "写穿活跃块"（hit_alloc_ra），定位才会落在分配函数体内。
        #      没有那条证据时，顶层位置在别处，这条规则自然不会命中。
        #
        #      规则本身可证伪：函数体里没有窄宽度乘法就 REFUTED。
        #      规则本身可证伪：函数体里没有窄宽度乘法就 REFUTED。
        #
        #      **用分配点位置，而不是崩溃点。**
        #      overflow_01 的乘法在 AllocateTable，崩溃在 FillTable 的 memset ——
        #      两个不同函数。若拿崩溃点去界定函数体，根本看不到乘法。
        #      分配点由 ZLAB_HIT_ALLOC_RA 给出、带 role="alloc"，
        #      它不参与 locations[0] 竞争，但正是这条规则要检查的地方。
        if code is not None:
            alloc_locs = [x for x in locations if getattr(x, "role", "crash") == "alloc"]
            for loc in alloc_locs:
                body = _function_body(code, loc)
                if body is None:
                    continue
                for bl in body:
                    if OverflowRule._ALLOC_RE.search(_strip_no(bl)):
                        out.append(OverflowRule(loc).to_hypothesis())
                        break

        # 2.5) 数据流追溯：写操作的长度参数是否下溢。
        #     这一条是规则推理超过纯栈分析的关键 ——
        #     它能把"在 memset 崩了"推进到"传给 memset 的 len 下溢了"。
        if code is not None and "ACCESS_VIOLATION" in exc:
            h = self._propose_underflow(facts, top)
            if h is not None:
                out.append(h)

        # 3) 栈深度很浅时，符号化可能不完整
        if 0 < len(facts.stack) < 3:
            out.append(Hypothesis(
                claim="栈帧过少，符号化可能不完整，结论可信度受限",
                kind="version",
            ))

        return out


# ============================================================ 内部工具

# 控制关键字不是函数名。
# 用于函数体界定时排除 if / while / switch 等被误判为函数定义的情况。
_CONTROL_KEYWORDS = {
    "if", "else", "for", "while", "switch", "do", "try", "catch",
    "sizeof", "return", "case", "default", "until", "foreach",
}


def _function_body(code: CodeIndex, loc: SourceLocation) -> list[str] | None:
    """
    取崩溃行所在函数体的全部行（带行号前缀）。

    向上找最近的 `{` 结尾的函数定义行作为起点，向下按括号配平找终点。
    朴素但够用：验证场景要的是「范围界定正确」，不是完整语法分析。
    返回 None 表示界定不了 —— 此时调用方应保持 UNPROVEN 而不是猜。

    **这里修掉一个影响所有验证规则的 bug。**

    原先的 def_re 会把控制流语句当成函数定义：
        if (mode == 2) {   -> 被当成函数 "if"
        while (x) {        -> 被当成函数 "while"
        switch (v) {       -> 被当成函数 "switch"
    实测 double_free 因此把函数体界定成 if 块的 3 行（75-77），
    漏掉了 74 行的第一次 free，于是"重复释放"被判成不存在而 REFUTED。

    后果是全局的：NullCheckRule 同样依赖函数体界定，
    若崩溃点在某个 if 块内，向上找保护时会被限制在块内，
    块外（真正的保护位置）的保护就看不见 —— 会把真漏洞判成"已有保护"。

    判据很简单：**控制关键字不是函数名**。这不需要语法分析器。
    """
    lines = code.read_lines(loc.path)
    if not lines:
        return None

    idx = loc.line - 1
    if not (0 <= idx < len(lines)):
        return None

    # 向上找函数定义起点：形如 "...(...)" 且行尾是 { 或下一行是 {
    def_re = re.compile(r"^\s*(?:[\w:<>\*&\s]+)\s+(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{?\s*$")

    def is_def(text: str) -> str | None:
        m = def_re.match(text)
        if not m:
            return None
        name = m.group(1)
        if name in _CONTROL_KEYWORDS:
            return None
        return name

    start = None
    for i in range(idx, -1, -1):
        if "{" in lines[i] and is_def(lines[i].split("{")[0] + "{"):
            start = i
            break
        if i > 0 and lines[i].strip() == "{" and is_def(lines[i - 1]):
            start = i - 1
            break
    if start is None:
        return None

    depth = 0
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > start:
            return [f"{n+1}: {lines[n]}" for n in range(start, i + 1)]

    return [f"{n+1}: {lines[n]}" for n in range(start, len(lines))]


def _guess_variable(code: CodeIndex, loc: SourceLocation) -> str | None:
    """
    从崩溃行推断被解引用的变量名。

    只认最常见的三种形态：p->x、p.x、(*p).x / p[i]。
    推断不出来就返回 None —— 无法验证就不要产出假设。
    """
    ctx = code.read_context(loc, radius=0)
    for line in ctx.splitlines():
        if _line_no(line) == loc.line:
            text = _strip_no(line)
            for pat in (r"(\w+)\s*->\s*\w+", r"\(\s*\*\s*(\w+)\s*\)\s*\.", r"(\w+)\s*\[\s*\w+\s*\]"):
                m = re.search(pat, text)
                if m:
                    return m.group(1)
    return None


def _split_args(s: str) -> list[str]:
    """
    按顶层逗号切分实参串（忽略括号/引号内的逗号）。

    朴素但够用：这里只需要拿到最后一个参数（长度），
    不需要完整解析 C++ 表达式。切不准返回空，调用方保持 UNPROVEN。
    """
    out: list[str] = []
    depth = 0
    cur = ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth < 0:
                break          # 调用表达式结束
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        cur += ch
    out.append(cur)
    return [x.strip() for x in out if x.strip()]


def _line_no(line: str) -> int | None:
    m = re.match(r"^\s*(\d+):", line)
    return int(m.group(1)) if m else None


def _strip_no(line: str) -> str:
    return re.sub(r"^\s*\d+:\s?", "", line)


# 函数定义行：形如 `Type Name(...) {`，末组是函数名。
_FUNC_DEF_RE = re.compile(
    r"^\s*(?:[\w:<>\*&\s]+)\s+(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{?\s*$")


def _enclosing_function_name(lines: list[str], line: int) -> str | None:
    """
    向上找最近的函数定义，返回其函数名。

    与 `_function_body` 用同一套界定（含排除 if/while 等控制关键字），
    否则两边会对"这个函数叫什么"给出不同答案。
    找不到返回 None —— 调用方应保持 UNPROVEN 而不是猜一个。
    """
    idx = line - 1
    if idx < 0 or idx >= len(lines):
        return None
    for i in range(idx, -1, -1):
        m = _FUNC_DEF_RE.match(lines[i].split("{")[0] + "{")
        if m and m.group(1) not in _CONTROL_KEYWORDS:
            return m.group(1)
    return None


def _function_params(lines: list[str], line: int,
                     func_name: str) -> list[str]:
    """
    取所在函数的参数名列表。

    终止条件"是否受外部控制"，判据就是条件里有没有出现参数名 ——
    常量封顶（kMaxDepth）不含参数，参数封顶（limit）含参数。
    取不到就返回空列表：调用方会因"没有参数可依赖"走向 REFUTED，
    那是保守方向，不会把正常递归误判成无限递归。
    """
    idx = line - 1
    if idx < 0 or idx >= len(lines):
        return []
    for i in range(idx, -1, -1):
        m = re.search(rf"\b{re.escape(func_name)}\s*\(([^)]*)\)", lines[i])
        if not m:
            continue
        out = []
        for p in m.group(1).split(","):
            p = p.strip()
            if not p:
                continue
            # 取最后一个标识符：`const int limit` -> `limit`
            mm = re.findall(r"\b(\w+)\b", p)
            if mm:
                out.append(mm[-1])
        return out
    return []


def _is_thread_entry(lines: list[str], func_name: str) -> bool:
    """
    该函数是否被作为线程入口启动。

    认三种最常见的形态：
      std::thread t(Fn, ...);        —— 直接构造
      ts.emplace_back(Fn, ...);      —— 容器里批量起
      std::async(Fn, ...)            —— 异步任务

    **只认"函数被传给线程"，不认"文件里出现了 thread"。**
    后者太宽：单线程程序也可能 include <thread> 却从不启动，
    那不构成并发访问。判据要能被推翻才有意义。
    """
    pat = re.compile(
        r"(?:std::thread\s*\(\s*" + re.escape(func_name)
        + r"|emplace_back\s*\(\s*" + re.escape(func_name)
        + r"|std::async\s*\(\s*" + re.escape(func_name)
        + r"|std::thread\s+\w+\s*\(\s*" + re.escape(func_name) + r")")
    for ln in lines:
        if pat.search(ln):
            return True
    return False


def _in_function(code: CodeIndex, path, line: int, func_name: str) -> bool:
    """
    判断某行是否位于指定函数体内。

    做法很朴素：向上找函数定义行，向下找括号配平。
    不做完整语法分析——朴素方法在验证场景下足够，且不会因为解析器不支持
    某些语言特性而误判。
    """
    lines = code.read_lines(path)
    if not lines or not (0 <= line - 1 < len(lines)):
        return False

    # 向上找函数定义行：行内含 "{" 且形如 "Type Name(...)"
    def_re = re.compile(rf"\b{re.escape(func_name)}\s*\([^;)]*\)\s*(?:const\s*)?\{{")

    # 同样要排除控制关键字：若 func_name 被误传成 if/while 之类，
    # 会把控制块当成函数体，进而把不在调用路径上的代码判成"在路径上"。
    if func_name in _CONTROL_KEYWORDS:
        return False

    start = None
    for i in range(min(line, len(lines)) - 1, -1, -1):
        if def_re.search(lines[i]):
            start = i
            break
        # 定义行与 { 分行写的情况
        if lines[i].strip() == "{" and i > 0 and re.search(
                rf"\b{re.escape(func_name)}\s*\(", lines[i - 1]):
            start = i - 1
            break
    if start is None:
        return False

    depth = 0
    for i in range(start, min(start + 400, len(lines))):
        depth += lines[i].count("{") - lines[i].count("}")
        if i >= line - 1 and depth > 0:
            return True
        if depth == 0 and i > start:
            return False
    return False
