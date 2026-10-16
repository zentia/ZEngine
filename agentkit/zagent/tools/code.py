"""
把 `CodeIndex` 协议映射成工具。

为什么是"映射"而不是重写
------------------------
`protocols.CodeIndex` 已经把代码读写与检索抽象干净了（git worktree +
grep + 行读取），验证规则全都跑在它上面。为 agent 再写一套读文件的
实现，等于把同一件事维护两遍，而且两边行为会悄悄分叉 —— 规则验证说
"第 88 行有下溢"，agent 读到的却是另一个版本的内容，这种分叉极难查。

所以这里只加壳：工具的实现直接转发给 CodeIndex。**行为完全一致**，
只是多了一层可被 LLM 枚举的入口。

read_lines 为什么默认限制行数
----------------------------
一个几千行的源文件全量塞回上下文，会挤掉模型真正需要的证据。这里默认
只给片段，并明确告诉模型"还有多少行没看到"，让它自己决定要不要再取 ——
把选择权交给模型，而不是替它截断后假装完整。
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from ..types import SourceLocation
from .base import STATUS_BAD_ARGS, STATUS_ERROR, Risk, Tool, ToolResult


def make_code_tools(code: Any, default_radius: int = 40,
                    max_lines: int = 400) -> list[Tool]:
    """从一个 CodeIndex 实现造出一组代码工具。"""

    def _read_context(path: str, line: int,
                      radius: int | None = None) -> ToolResult:
        # **必须转成 Path**：`SourceLocation.path` 与 `CodeIndex` 各方法
        # 的签名都是 Path，而工具入参是 JSON 里的字符串。直接传 str，
        # 在桩实现上能跑通（桩不碰 path），在真实 GitCodeIndex 上会在
        # 第一次 `path.read_text()` 处炸 —— 这类"只在真实实现上暴露"的
        # 类型错误，正是工具层最容易漏掉的一类。
        try:
            loc = SourceLocation(path=Path(path), line=int(line))
        except (TypeError, ValueError, OSError) as e:
            return ToolResult(ok=False, status=STATUS_BAD_ARGS,
                              error=f"无法解析位置 {path}:{line} ({e})")
        if loc.line < 1:
            return ToolResult(ok=False, status=STATUS_BAD_ARGS,
                              error="line 必须 >= 1")
        try:
            text = code.read_context(loc, radius=radius or default_radius)
        except Exception as e:  # noqa: BLE001
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"读取失败: {type(e).__name__}: {e}")
        if not text:
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"{path}:{line} 无内容（文件不存在或越界）")
        return ToolResult(ok=True, output=text,
                          data={"path": str(loc.path), "line": loc.line})

    def _read_lines(path: str, start: int = 1,
                    count: int = max_lines) -> ToolResult:
        p = Path(path)
        try:
            lines = code.read_lines(p)
        except Exception as e:  # noqa: BLE001
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"读取失败: {type(e).__name__}: {e}")
        if not lines:
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"{path} 为空或不存在")

        total = len(lines)
        s = max(1, int(start))
        c = max(1, min(int(count), max_lines))
        chunk = lines[s - 1: s - 1 + c]
        body = "\n".join(f"{s + i}: {t}" for i, t in enumerate(chunk))

        hidden = total - (s - 1 + len(chunk))
        note = f"\n[共 {total} 行，本次显示 {s}~{s + len(chunk) - 1}" \
               + (f"，还有 {hidden} 行未显示]" if hidden > 0 else "]")
        return ToolResult(ok=True, output=body + note,
                          data={"path": str(p), "total": total,
                                "shown": len(chunk), "hidden": max(0, hidden)})

    def _grep(pattern: str, limit: int = 50) -> ToolResult:
        if not pattern:
            return ToolResult(ok=False, status=STATUS_BAD_ARGS,
                              error="pattern 为空")
        try:
            hits = code.grep(pattern)
        except Exception as e:  # noqa: BLE001
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"检索失败: {type(e).__name__}: {e}")
        if not hits:
            return ToolResult(ok=True, output=f"未匹配到 {pattern!r}",
                              data={"count": 0})

        lines = [f"{p}:{ln}: {text.strip()}" for p, ln, text in hits[:limit]]
        out = "\n".join(lines)
        truncated = len(hits) > limit
        if truncated:
            out += f"\n[共 {len(hits)} 处，仅显示前 {limit} 处]"
        return ToolResult(ok=True, output=out, truncated=truncated,
                          data={"count": len(hits)})

    def _revision() -> ToolResult:
        try:
            rev = code.current_revision()
        except Exception as e:  # noqa: BLE001
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"获取失败: {type(e).__name__}: {e}")
        return ToolResult(ok=True, output=f"{rev.id}" + (f" ({rev.note})"
                                                         if rev.note else ""),
                          data={"id": rev.id, "note": rev.note})

    return [
        Tool(
            name="read_context",
            description="读取指定文件某行附近的上下文（带行号）。定位后看现场用这个。",
            risk=Risk.READ,
            params={
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "line": {"type": "integer"},
                    "radius": {"type": "integer", "description": "上下各取多少行"},
                },
                "required": ["path", "line"],
            },
            fn=_read_context,
        ),
        Tool(
            name="read_lines",
            description=f"按区间读取文件行（带行号，默认最多 {max_lines} 行）。"
                        f"需要通读函数体时用这个。",
            risk=Risk.READ,
            params={
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "start": {"type": "integer", "default": 1},
                    "count": {"type": "integer", "default": max_lines},
                },
                "required": ["path"],
            },
            fn=_read_lines,
        ),
        Tool(
            name="grep",
            description="按正则在代码库中检索，返回 文件:行号:内容。",
            risk=Risk.SEARCH,
            params={
                "type": "object",
                "properties": {
                    "pattern": {"type": "string", "description": "正则表达式"},
                    "limit": {"type": "integer", "default": 50},
                },
                "required": ["pattern"],
            },
            fn=_grep,
        ),
        Tool(
            name="current_revision",
            description="取当前代码版本（revision）。版本对齐门禁要用它。",
            risk=Risk.READ,
            params={"type": "object", "properties": {}},
            fn=_revision,
        ),
    ]
