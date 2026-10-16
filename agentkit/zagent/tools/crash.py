"""
把崩溃诊断**降级成一个工具**。

这是整个演进里最关键的一步，方向很容易搞反：不是把 `diagnose()` 改写成
agent，而是让 agent 去调用它。

为什么必须降级而不是改写
------------------------
`diagnose()` 是一条固定五阶段流水线，它的价值恰恰在于**确定性** ——
同样的输入永远走同样的步骤，所以可回归、可 eval。一旦把 LLM 塞进它的
阶段里，这个性质就没了，而你会同时失去基线和归因能力。

降级之后两者兼得：
- 流水线保住确定性（可 eval、可回归）
- agent 拿到自主性（**决定何时调用**、调用之后干什么）

关键收益体现在**版本不一致**这个场景上，这是最典型的分水岭：

- 旧行为：`auto_checkout=False` → 硬 `return report`，到此为止。没有
  任何通道能继续 —— 这正是"自动化工具"和"agent"的分界。
- 新行为：`diagnose_crash` 返回 `blocked` 并说明原因，**agent 读到这条
  观测后自己决定下一步**：去调 shell 切分支、切完再调一次。

同一个护栏，从"死路"变成了"可绕过的路障"，而护栏本身一个字没改。

风险等级为什么定 READ
---------------------
`diagnose()` 唯一的副作用是写 JSONL 轨迹日志，不碰工作区、不碰代码。
这里**强制 `auto_checkout=False`**：自动切分支会改动开发者的 working
tree，那是破坏性操作，该由 agent 显式调用 shell 去做，并走权限审批 ——
不能藏在一个"只读诊断"工具里偷偷做掉。
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable

from ..pipeline import diagnose, render
from ..protocols import Context
from ..tracing import Tracer
from .base import STATUS_BAD_ARGS, STATUS_ERROR, Risk, Tool, ToolResult


def make_crash_tools(
    resolve_ctx: Callable[[str], Context | None],
    trace_dir: Path | None = None,
) -> list[Tool]:
    """
    造崩溃诊断工具。

    resolve_ctx
        由 ref（崩溃标识）解析出一个装配好的 `Context`。
        **为什么是回调而不是直接传 Context**：每个 ref 对应的事实来源
        不同（不同的 dump 文件），而 `LocalDumpSource` 是在构造时绑定
        文件的。传固定 Context 会让所有 ref 都读同一个 dump —— 这类
        错误非常隐蔽，因为流水线不会报错，只会给出一个"看起来合理"的
        结论。eval 里它会让所有样本得分一模一样。
    """

    def _diagnose(ref: str) -> ToolResult:
        if not ref or not str(ref).strip():
            return ToolResult(ok=False, status=STATUS_BAD_ARGS,
                              error="ref 为空")

        ctx = resolve_ctx(str(ref))
        if ctx is None:
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"无法为 ref={ref!r} 装配诊断上下文")

        missing = ctx.ready_check()
        if missing:
            return ToolResult(
                ok=False, status="blocked",
                output=f"能力不可用: {', '.join(missing)}",
                error=f"能力不可用: {', '.join(missing)}",
                data={"missing": list(missing)})

        report = diagnose(str(ref), ctx, Tracer(f"tool-{ref}", trace_dir),
                          auto_checkout=False)

        # 把 Report 对象本身放进 data：eval 要靠它复用既有的 score()。
        # 只回文本的话，agent 级 eval 就没法跟流水线基线直接比数字 ——
        # 而"能不能直接比"是这次接 eval 的全部意义所在。
        return ToolResult(
            ok=report.status != "blocked",
            output=render(report),
            status=report.status,
            error="" if report.status != "blocked" else report.block_reason,
            data={
                "report": report,
                "status": report.status,
                "location": str(report.location) if report.location else "",
                "confidence": report.confidence,
                "unknowns": list(report.unknowns),
            },
        )

    def _ready() -> ToolResult:
        """
        诊断能力自检。

        **为什么值得单独做一个工具**：agent 被阻塞时，如果没有这个工具，
        它只能对着一句"能力不可用"反复重试；有了它，它能先查出缺什么
        （source / version / code），再决定是去修配置还是直接告诉用户。
        把"为什么做不到"变成可查询的，是 agent 能自我纠错的前提。
        """
        ctx = resolve_ctx("")
        if ctx is None:
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error="无法装配诊断上下文")
        missing = ctx.ready_check()
        return ToolResult(
            ok=not missing,
            output=("全部就绪" if not missing
                    else f"缺失能力: {', '.join(missing)}"),
            data={"missing": list(missing)},
        )

    return [
        Tool(
            name="diagnose_crash",
            description=(
                "诊断一次崩溃，返回定位结论。版本不一致时会返回 blocked 并说明"
                "原因，此时应先自行切换代码（调 shell 执行 git checkout <rev>）"
                "再重试 —— 本工具不会替你自动切分支。"
            ),
            risk=Risk.READ,
            params={
                "type": "object",
                "properties": {
                    "ref": {"type": "string",
                            "description": "崩溃标识（dump 引用）"},
                },
                "required": ["ref"],
            },
            fn=_diagnose,
        ),
        Tool(
            name="crash_ready",
            description="检查崩溃诊断能力是否就绪，返回缺失的能力名。",
            risk=Risk.READ,
            params={"type": "object", "properties": {}},
            fn=_ready,
        ),
    ]
