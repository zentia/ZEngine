"""
把 `Verifier` 暴露成工具。这是「既自主又不幻觉」的落点。

分工
----
- **模型决定何时验证**：看到可疑的地方就去调，调几次、验证什么，由它定。
- **规则决定怎样算验证过**：8 条规则把持裁定权，模型无法自己给自己发
  CONFIRMED。

这就是和玩具 ReAct demo 的分界线：玩具 agent 的"验证"是模型自己再说一遍
"我确认了"，那等于没验证。这里的裁定发生在模型之外。

三个必须做对的语义
------------------
1. **REFUTED 不是失败。**
   验证成功了、答案是"不成立"，这是最有价值的结果之一。
   如果把它回成 `ok=False`，模型会当成"工具出错"去重试 —— 恰恰是把
   正确结论当成故障。所以三种裁定都 `ok=True`，用 `status` 区分。

2. **UNPROVEN 也不是失败，但绝不能进结论。**
   "无法判断"是合法结论，"猜一个"不是（既有 `RuleVerifier` 的注释原文）。
   工具返回 ok=True，同时在输出里明确标注不可进结论。

3. **台账（ledger）要留全。**
   被推翻的假设也要留着 —— 既有设计里 `Report.ledger` 保存全量含 REFUTED，
   这是"可归因"的前提：事后能看出"试过什么、为什么否掉"。
   这里同理，verify 过的全部进 ledger，`conclusion_ledger` 只吐 CONFIRMED。

关于"结论门禁"
--------------
理想情况应该强制：没经过 CONFIRMED 的假设不许写进 `finish` 的结论。
当前靠 system prompt 约束 + `conclusion_ledger` 提供自检手段，**尚未在
finish 处硬拦**（那需要 loop 层持有台账，见路线图）。
这里不做"假装已经拦住了"的事 —— 约束到哪一步就说到哪一步。
"""

from __future__ import annotations

from typing import Any

from ..types import Hypothesis, Verdict
from .base import STATUS_BAD_ARGS, Risk, Tool, ToolResult

# 每条规则重建所需的参数。
# 与 `RuleVerifier._rules` 里各 `from_data` 的取值一一对应：
# 参数不齐时 `from_data` 返回 None，规则会被判为 UNPROVEN ——
# 与其让模型瞎猜参数然后拿到一个"看起来验证过了"的空结果，
# 不如在工具层就把需要的参数说清楚。
RULE_PARAMS: dict[str, tuple[str, ...]] = {
    "callchain": ("caller", "callee"),
    "nullcheck": ("path", "line", "variable"),
    "unsigned_underflow": ("path", "line", "size_arg"),
    "double_free": ("path", "line"),
    "leak": ("path", "line"),
    "race": ("path", "line", "variable"),
    "recursion": ("path", "line", "func_name"),
    "overflow": ("path", "line"),
}

RULE_HINT: dict[str, str] = {
    "callchain": "调用链：caller 函数体内是否调用了 callee",
    "nullcheck": "空指针：崩溃行所在函数体内，该变量是否有保护",
    "unsigned_underflow": "无符号下溢：size_arg 的赋值点是否可能算出负数",
    "double_free": "重复释放：同一指针是否被释放两次",
    "leak": "泄漏：分配后是否存在未释放的早退路径",
    "race": "竞争：该变量是否被多线程无保护访问",
    "recursion": "递归：是否有终止条件 / 是否无界递归",
    "overflow": "栈溢出：局部变量是否过大或递归过深",
}


def make_verify_tools(code: Any, verifier: Any) -> list[Tool]:
    """
    造验证工具。

    ledger 是工厂闭包里的共享列表：**所有 verify 过的假设都进去**，
    包括被推翻的。`conclusion_ledger` 只吐 CONFIRMED 的。
    这样事后能回答"它试过什么、否掉了什么" —— 可归因的前提。
    """
    ledger: list[Hypothesis] = []

    def _verify(kind: str, claim: str,
                data: dict[str, Any] | None = None) -> ToolResult:
        kind = (kind or "").strip()
        if kind not in RULE_PARAMS:
            return ToolResult(
                ok=False, status=STATUS_BAD_ARGS,
                error=f"未知假设类型 {kind!r}。可用: {', '.join(sorted(RULE_PARAMS))}")

        required = RULE_PARAMS[kind]
        data = dict(data or {})
        missing = [k for k in required if not data.get(k)]
        if missing:
            return ToolResult(
                ok=False, status=STATUS_BAD_ARGS,
                error=f"{kind} 缺少参数 {missing}。"
                      f"需要: {', '.join(required)}")

        h = Hypothesis(claim=claim or f"{kind}@{data.get('path', '')}",
                       kind=kind, data=data)
        try:
            h = verifier.check(h, code)
        except Exception as e:  # noqa: BLE001 —— 规则炸了也是一条观测
            return ToolResult(ok=False, status="error",
                              error=f"规则执行异常: {type(e).__name__}: {e}")

        ledger.append(h)

        v = h.verdict
        status = (STATUS_CONFIRMED if v == Verdict.CONFIRMED
                  else STATUS_REFUTED if v == Verdict.REFUTED
                  else STATUS_UNPROVEN)

        lines = [f"裁定: {v.value}", f"假设: {h.claim}"]
        if h.note:
            lines.append(f"说明: {h.note}")
        if h.evidence:
            lines.append("证据:")
            for e in h.evidence:
                lines.append(f"  - [{e.method}] {e.locator}  {e.detail}")
        else:
            lines.append("证据: 无（无证据的裁定不可进结论）")

        # 进结论的门槛：CONFIRMED **且** 有可回溯证据。
        # 少了后一半，一条"我说它对"也能混进结论 —— CONFIRMED 是必要不充分。
        traceable = [e for e in h.evidence if e.is_traceable]
        allowed = v == Verdict.CONFIRMED and bool(traceable)
        lines.append(f"可否进结论: {'是' if allowed else '否'}")

        return ToolResult(
            ok=True,                 # 三种裁定都是"验证成功完成"
            output="\n".join(lines),
            status=status,
            data={"hypothesis": h, "verdict": v.value,
                  "allowed_in_conclusion": allowed,
                  "evidence": [e.locator for e in h.evidence]},
        )

    def _rules() -> ToolResult:
        lines = []
        for kind in sorted(RULE_PARAMS):
            lines.append(f"  {kind:<20} 参数: {', '.join(RULE_PARAMS[kind])}"
                         f"   {RULE_HINT.get(kind, '')}")
        return ToolResult(ok=True, output="\n".join(lines),
                          data={"kinds": sorted(RULE_PARAMS)})

    def _ledger() -> ToolResult:
        if not ledger:
            return ToolResult(ok=True, output="还没有验证过任何假设",
                              data={"confirmed": [], "refuted": [],
                                    "unproven": []})

        confirmed = [h for h in ledger if h.verdict == Verdict.CONFIRMED]
        refuted = [h for h in ledger if h.verdict == Verdict.REFUTED]
        unproven = [h for h in ledger if h.verdict == Verdict.UNPROVEN]

        lines = [f"已验证 {len(ledger)} 条假设："
                 f"CONFIRMED {len(confirmed)} / "
                 f"REFUTED {len(refuted)} / "
                 f"UNPROVEN {len(unproven)}", ""]
        lines.append("可进结论（CONFIRMED 且有可回溯证据）:")
        for h in confirmed:
            if any(e.is_traceable for e in h.evidence):
                lines.append(f"  + {h.claim}")
        if not any(any(e.is_traceable for e in h.evidence) for h in confirmed):
            lines.append("  （无）")

        if refuted:
            lines.append("")
            lines.append("已被推翻（保留在台账，不进结论）:")
            for h in refuted:
                lines.append(f"  - {h.claim}")

        return ToolResult(
            ok=True, output="\n".join(lines),
            data={"confirmed": [h.claim for h in confirmed],
                  "refuted": [h.claim for h in refuted],
                  "unproven": [h.claim for h in unproven]},
        )

    return [
        Tool(
            name="verify_hypothesis",
            description=(
                "用领域规则验证一条假设，返回 CONFIRMED / REFUTED / UNPROVEN "
                "与可回溯证据。**裁定由规则做出，不由你做出** —— "
                "REFUTED 是有价值的结果，不要当成失败去重试。"
                "先调 list_verify_rules 看有哪些类型和各自需要什么参数。"
            ),
            risk=Risk.READ,
            params={
                "type": "object",
                "properties": {
                    "kind": {"type": "string",
                             "description": "假设类型，如 nullcheck / leak"},
                    "claim": {"type": "string",
                              "description": "可证伪的断言，不要写'可能是内存问题'"},
                    "data": {"type": "object",
                             "description": "规则参数，如 {path, line, variable}"},
                },
                "required": ["kind", "claim"],
            },
            fn=_verify,
        ),
        Tool(
            name="list_verify_rules",
            description="列出可用的假设类型，以及每种类型需要的参数。",
            risk=Risk.READ,
            params={"type": "object", "properties": {}},
            fn=_rules,
        ),
        Tool(
            name="conclusion_ledger",
            description=(
                "查看已验证过的假设台账：哪些可进结论、哪些已被推翻。"
                "收尾前调一次，确认结论里每一条都有 CONFIRMED 支撑。"
            ),
            risk=Risk.READ,
            params={"type": "object", "properties": {}},
            fn=_ledger,
        ),
    ]


STATUS_CONFIRMED = "confirmed"
STATUS_REFUTED = "refuted"
STATUS_UNPROVEN = "unproven"
