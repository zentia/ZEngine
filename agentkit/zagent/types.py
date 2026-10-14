"""
核心数据类型。

设计要点：
1. 事实与推断严格分离。Facts 只装从数据源取到的东西，Hypothesis 只装推断。
2. 缺字段显式记录，不允许用默认值假装存在。
3. 证据三态：CONFIRMED / REFUTED / UNPROVEN。UNPROVEN 允许存在但拉低置信度，
   REFUTED 保留在台账里体现纠偏过程。
"""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any


class Verdict(str, Enum):
    """假设验证结论。三态而非二态，是为了允许「看起来对但无法锁定」这种中间态。"""

    CONFIRMED = "confirmed"   # 有实证支撑
    REFUTED = "refuted"       # 被证据推翻
    UNPROVEN = "unproven"     # 兼容但无法锁定


@dataclass(frozen=True)
class Revision:
    """代码版本标识。不绑定具体 VCS。"""

    id: str
    note: str = ""


@dataclass
class SourceLocation:
    path: Path
    line: int
    context: str = ""        # 崩溃行附近上下文
    symbol: str = ""         # 符号名（若已解析）

    # **位置在定位决策里的身份。**
    #
    # 之前所有位置一视同仁，谁排在 locations 前面谁就是 locations[0]，
    # 而 `_pick_location` 在非递归栈下直接返回 locations[0]。
    # 于是"分配点"这种**补充证据**一旦插在崩溃点之前，
    # 就会顶替崩溃点成为报告定位 —— oob_write 因此被带偏 8 行：
    # 实测定位到 main 里的 `ZLabAlloc(64)`（487），
    # 而根因是 `len = r.end - r.start` 的下溢（479）。
    #
    # 崩溃点是实测事实，分配点是推断线索，二者不能同等竞争。
    # role 让"谁有资格当 locations[0]"成为显式约定，而不是插入顺序的副产品。
    role: str = "crash"      # crash = 崩溃现场；alloc = 分配点（仅作补充证据）

    def __str__(self) -> str:
        return f"{self.path}:{self.line}"

    def same_file(self, other: "SourceLocation | None") -> bool:
        return other is not None and self.path == other.path

    def same_line(self, other: "SourceLocation | None") -> bool:
        return other is not None and (self.path, self.line) == (other.path, other.line)


@dataclass
class StackFrame:
    """原始栈帧。未经解释的事实。"""

    module: str = ""
    address: int | None = None
    function: str = ""
    file: str = ""
    line: int | None = None
    is_project_code: bool = False

    # 该帧在定位决策里的身份，会带到 SourceLocation.role 上。
    # "alloc" 表示这是**分配点**（推断线索），不是崩溃现场。
    # 探测器产出 ZLAB_HIT_ALLOC_RA 时才会带上它 ——
    # 没有那条运行时证据的帧一律是普通崩溃帧。
    role: str = "crash"


@dataclass
class Facts:
    """
    崩溃事实。只装从数据源取到的东西，不做任何推断。

    missing 是关键设计：取不到的字段必须显式记录，它会一路传播到最终置信度。
    用默认值假装字段存在，是这类流水线最容易犯的错。
    """

    ref: str = ""
    platform: str = ""
    product_version: str = ""

    # 构建身份硬证据：PDB GUID + Age。由引擎侧 CrashHandler 写入快照。
    #
    # 为什么它比 product_version 更可信：版本串是人维护的，改了代码忘记
    # bump 是常态；build_id 是链接器生成的，同一次链接才相同。Release 构建
    # 没有行号时，它是把崩溃帧对回源码的唯一硬抓手。
    # 拿不到时 build_id 为空、build_id_reason 说明原因（如
    # "debug_directory_without_codeview"）—— 绝不用空串蒙混。
    build_id: str = ""
    build_id_reason: str = ""
    build_timestamp: int = 0
    exception_type: str = ""
    exception_message: str = ""
    stack: list[StackFrame] = field(default_factory=list)
    crash_time: str = ""
    uid: str = ""
    device: str = ""
    uptime_seconds: float | None = None
    missing: list[str] = field(default_factory=list)

    def present_fields(self) -> list[str]:
        out = []
        for name in ("platform", "product_version", "build_id",
                     "exception_type", "stack",
                     "crash_time", "uid", "device", "uptime_seconds"):
            v = getattr(self, name)
            if v not in (None, "", [], {}):
                out.append(name)
        return out

    def note_missing(self, *names: str) -> None:
        for n in names:
            if n not in self.missing:
                self.missing.append(n)

    def has_frames(self) -> bool:
        return bool(self.stack)


@dataclass
class Evidence:
    """
    一条证据。method 说明怎么验证的，locator 必须可回溯。

    不允许出现没有 locator 的证据——那种「我觉得是这样」在根因定位里是致命的。
    """

    method: str                    # read_code / grep / addr2line / compute_layout / run_command
    locator: str = ""              # 文件:行号 / 命令 / 计算过程
    detail: str = ""

    @property
    def is_traceable(self) -> bool:
        return bool(self.locator.strip())

    def __str__(self) -> str:
        return f"[{self.method}] {self.locator}" + (f" — {self.detail}" if self.detail else "")


@dataclass
class Hypothesis:
    """
    一条假设。写进结论前必须经过 verify。

    claim 要具体到可证伪：「崩溃点在分配器而非破坏点」可以证伪，
    「可能是内存问题」不可证伪，后者不该进台账。
    """

    id: str = field(default_factory=lambda: uuid.uuid4().hex[:8])
    claim: str = ""
    kind: str = ""                 # callchain / nullcheck / lifetime / ownership / thread / version
    evidence: list[Evidence] = field(default_factory=list)
    verdict: Verdict = Verdict.UNPROVEN
    note: str = ""
    data: dict[str, Any] = field(default_factory=dict)  # 规则参数，供验证器重建规则

    def confirm(self, ev: Evidence) -> "Hypothesis":
        self.evidence.append(ev)
        self.verdict = Verdict.CONFIRMED
        return self

    def refute(self, ev: Evidence) -> "Hypothesis":
        self.evidence.append(ev)
        self.verdict = Verdict.REFUTED
        return self

    @property
    def is_conclusion(self) -> bool:
        """能否进入结论。REFUTED 绝对不行——这是硬护栏。"""
        return self.verdict in (Verdict.CONFIRMED, Verdict.UNPROVEN)


@dataclass
class Report:
    """诊断报告。"""

    run_id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    status: str = "ok"            # ok / blocked / failed
    block_reason: str = ""
    location: SourceLocation | None = None
    conclusions: list[Hypothesis] = field(default_factory=list)
    ledger: list[Hypothesis] = field(default_factory=list)   # 全量，含被推翻的
    unknowns: list[str] = field(default_factory=list)
    confidence: float = 0.0
    facts: Facts | None = None
    elapsed_ms: int = 0

    @property
    def ok(self) -> bool:
        return self.status == "ok"

    def top(self, n: int = 1) -> list[SourceLocation]:
        """取前 n 个根因候选位置，供 eval 计算 top-k。"""
        locs: list[SourceLocation] = []
        for h in self.conclusions:
            for ev in h.evidence:
                loc = _parse_locator(ev.locator)
                if loc and all(not loc.same_line(x) for x in locs):
                    locs.append(loc)
                    break
            if len(locs) >= n:
                break
        return locs[:n]


def _parse_locator(s: str) -> SourceLocation | None:
    """从 'path/to/file.cpp:123' 形式解析位置。解析不出来返回 None，不猜。"""
    if not s or ":" not in s:
        return None
    head, _, tail = s.rpartition(":")
    if not tail.strip().lstrip("-").isdigit():
        return None
    try:
        return SourceLocation(path=Path(head), line=int(tail))
    except (ValueError, OSError):
        return None


def compute_confidence(conclusions: list[Hypothesis],
                       missing_fields: list[str]) -> float:
    """
    置信度。设计原则：宁可低估。

    - 只有 UNPROVEN 没有 CONFIRMED 时，压到很低
    - 缺字段按权重扣
    - 证据不可回溯的按条扣
    """
    if not conclusions:
        return 0.0

    confirmed = sum(1 for h in conclusions if h.verdict == Verdict.CONFIRMED)
    unproven = sum(1 for h in conclusions if h.verdict == Verdict.UNPROVEN)

    if confirmed == 0:
        base = 0.15          # 全是未证实，几乎不可信
    else:
        base = 0.45 + 0.25 * min(confirmed, 2)
        base -= 0.10 * unproven

    # 缺字段惩罚
    penalty = 0.06 * len(missing_fields)

    # 不可回溯证据惩罚。
    # 权重要够重：一条不可回溯的证据意味着这条结论无法被复核，
    # 在根因定位里这几乎等同于没查。轻罚会让「猜」看起来和「查」差不多。
    untraceable = sum(
        1 for h in conclusions for e in h.evidence if not e.is_traceable
    )
    penalty += 0.25 * untraceable

    return round(max(0.0, min(1.0, base - penalty)), 2)


def now_ms() -> int:
    return int(time.time() * 1000)
