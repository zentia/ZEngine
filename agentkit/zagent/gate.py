"""
结论硬门禁：在 `finish` 处拦截没有证据支撑的结论。

为什么必须是硬门禁
------------------
上一版只做了两件事：system prompt 里写"不许瞎编"，再给一个
`conclusion_ledger` 让模型自检。两者都依赖模型自觉 —— 而模型不遵守
prompt 恰恰是我们要防的那种故障。**靠被防的对象自觉遵守规则，不算门禁。**

这里把它变成控制流：结论被拒，这一步就结束不了，模型必须改。

为什么它是不可绕过的
--------------------
允许收尾的结论集合 = 轨迹里出现过、且被规则判为 CONFIRMED、且带可回溯
证据的那些 claim。**模型无法自己往这个集合里加东西** —— 它只能调用
`verify_hypothesis`，而裁定权在规则手里。

所以模型凭空写一条结论时，它必然匹配不上任何 CONFIRMED 条目，门禁必然
拒绝。这不是"希望它不要瞎编"，是"它编了就会被拦"。

门禁规则（二分，不给第三条路）
------------------------------
- **要下结论** -> 每一条都必须匹配到 CONFIRMED + 可回溯证据；
- **不下结论** -> 必须说明没查清什么（`unknowns` 非空）。

"既不下结论、也不说不知道"在严格模式下会被拒。这正是不诚实最常见的
形态：给一段听起来专业的话，不承担任何可核查的断言。

`require_confirmed=0`（默认）只启用前两条；设为 1 会连第三条一起启用，
适用于"这次就是要一个诊断结论"的任务。

匹配为什么是模糊的
------------------
模型声明的结论和规则生成的 claim 措辞不会完全一致。用精确匹配会让
正确的结论也被拒 —— 门禁过严的代价是 agent 卡死，那比漏过一条更糟。
这里用「归一化后包含 / difflib 相似度」，阈值取 0.62。
短句（<8 字符）不参与包含匹配，否则"是""没有"这种词能匹配一切。

刻意没做的事
------------
**不解析 answer 的自然语言。** 从一段话里判断"它是不是在断言"做不准，
做不准的门禁会让正确收尾被拒，用户会觉得这工具莫名其妙。门禁只管模型
**显式声明**的结论 —— 声明是它自己给的，所以判定是确定的。
"""

from __future__ import annotations

import difflib
import re
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # 只用于类型标注：运行时不能 import loop，否则成环
    from .loop import Finish, Turn

STATUS_GATE_REJECTED = "gate_rejected"

CONFIRMED = "confirmed"
REFUTED = "refuted"
UNPROVEN = "unproven"


@dataclass
class Claim:
    """一条被规则裁定过的假设。"""

    claim: str
    verdict: str
    locators: list[str] = field(default_factory=list)

    @property
    def allowed(self) -> bool:
        """可进结论：CONFIRMED **且**带可回溯证据。两者缺一不可。"""
        return self.verdict == CONFIRMED and bool(self.locators)


def collect_claims(history: list[Any]) -> list[Claim]:
    """
    从轨迹里收集所有被裁定过的假设。

    **刻意做成扫描观测而不是持有台账**：任何工具只要在自己的
    `ToolResult.data` 里给出 `verdict`（单条）或 `conclusions`（多条），
    就自动参与门禁。这样 loop 不需要认识 `verify_hypothesis`，
    也不需要认识 `diagnose_crash` —— 将来加新能力，门禁自动生效。
    认识具体工具的门禁，每加一个能力就要改一次，那是会烂掉的设计。
    """
    out: list[Claim] = []

    for t in history:
        obs = getattr(t, "observation", None)
        data = getattr(obs, "data", None) or {}
        if not isinstance(data, dict):
            continue

        if data.get("verdict"):
            hyp = data.get("hypothesis")
            text = getattr(hyp, "claim", "") or str(data.get("claim", ""))
            if text:
                out.append(Claim(claim=str(text),
                                 verdict=str(data["verdict"]),
                                 locators=list(data.get("evidence") or [])))

        for c in data.get("conclusions") or []:
            if isinstance(c, dict) and c.get("verdict"):
                out.append(Claim(claim=str(c.get("claim", "")),
                                 verdict=str(c.get("verdict")),
                                 locators=list(c.get("locators") or [])))

    return out


def _norm(s: str) -> str:
    return re.sub(r"\s+", " ", str(s or "")).strip().casefold()


def matches(declared: str, verified: str, threshold: float = 0.62,
            coverage: float = 0.8) -> bool:
    """
    声明的结论是否对应某条已验证的 claim。

    **为什么除了字符级 ratio 还要一条覆盖率判据**：模型声明的结论几乎
    总比规则生成的 claim 短（"a 缺少空判断" vs
    "a 在 actor.cpp:2 处使用前缺少空判断"）。而 difflib 的 ratio 分母是
    两串长度之和，长度差一大就被压到阈值以下 —— 结果是正确的简写被判成
    "没验证过"。这是实测踩到的，不是推演。

    门禁过严的代价是 agent 卡死（一直改一直被拒，最后 stalled），
    那比漏过一条可疑结论更糟，所以覆盖率判据必须补上。
    """
    a, b = _norm(declared), _norm(verified)
    if not a or not b:
        return False
    if a == b:
        return True
    if min(len(a), len(b)) >= 8 and (a in b or b in a):
        return True

    sm = difflib.SequenceMatcher(None, a, b)
    if sm.ratio() >= threshold:
        return True

    # 覆盖率：较短串有多大比例被较长串覆盖。
    # 下限 6 个字符是防"是""没有"这种碎片靠单字命中一切。
    m = sum(bl.size for bl in sm.get_matching_blocks())
    return min(len(a), len(b)) >= 6 and m / min(len(a), len(b)) >= coverage


@dataclass
class GateDecision:
    allowed: bool
    reason: str = ""


class ConclusionGate:
    """
    结论门禁。

    require_confirmed
        要求至少 N 条已验证结论。0 = 只拦"瞎编"，允许"不下结论"。
        1 = 还额外要求：不下结论就必须说明没查清什么。
    """

    def __init__(self, require_confirmed: int = 0,
                 threshold: float = 0.62) -> None:
        self.require_confirmed = require_confirmed
        self.threshold = threshold

    def check(self, finish: "Finish", history: list["Turn"]) -> GateDecision:
        claims = collect_claims(history)
        permitted = [c for c in claims if c.allowed]
        refuted = [c for c in claims if c.verdict == REFUTED]

        declared = [str(c).strip()
                    for c in (getattr(finish, "conclusions", None) or [])
                    if str(c).strip()]
        unknowns = list(getattr(finish, "unknowns", None) or [])

        # 1) 声明撞上已被推翻的：最严重，单独报。
        #    模型如果把它写进结论，说明它没读懂 REFUTED 的含义。
        for d in declared:
            hit = next((r for r in refuted
                        if matches(d, r.claim, self.threshold)), None)
            if hit is not None:
                return GateDecision(
                    False,
                    f"结论 {d!r} 已被规则推翻（REFUTED）：{hit.claim}。"
                    f"被推翻的假设不许写进结论，请改结论或改去查别的方向。")

        # 2) 每条声明都必须有 CONFIRMED + 可回溯证据
        unbacked = [d for d in declared
                    if not any(matches(d, p.claim, self.threshold)
                               for p in permitted)]
        if unbacked:
            avail = "\n".join(f"    - {p.claim}" for p in permitted)
            return GateDecision(
                False,
                f"以下结论没有已验证的证据支撑：{unbacked}。"
                f"可用（CONFIRMED 且带可回溯证据）的结论只有：\n{avail or '    （无）'}\n"
                f"要么改去调用 verify_hypothesis 验证它，要么把它放进 unknowns。")

        # 3) 数量不够
        if len(declared) < self.require_confirmed:
            if declared:
                return GateDecision(
                    False, f"需要至少 {self.require_confirmed} 条已验证结论，"
                           f"当前只有 {len(declared)} 条。")
            if not unknowns:
                return GateDecision(
                    False,
                    "既没有给出任何已验证结论，也没有说明没查清什么。"
                    "要么去验证，要么如实列出 unknowns —— 不允许既不结论也不承认不知道。")

        return GateDecision(True)
