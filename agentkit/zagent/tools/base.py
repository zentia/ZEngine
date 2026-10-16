"""
工具层基座：Risk / ToolResult / Tool / ToolRegistry。

三条设计约束，都是被"agent 会怎么死"逼出来的：

1. **调用永不抛异常。**
   工具失败必须是 ToolResult 而不是 raise。agent loop 里一次未捕获异常
   会直接终结整轮执行，而"这个工具的参数用错了"本该是一条模型能看到、
   并据此纠正的观测。raise 把可恢复的错误变成了致命错误。

2. **结果必须结构化。**
   `status` 给程序读（策略层据此裁决、loop 据此判断是否陷入停滞），
   `output` 给模型读。两者混在一段字符串里，"卡住了"和"失败了"就无法
   区分，而这两者的下一步动作完全不同。

3. **风险等级挂在工具上，不挂在调用点。**
   权限裁决因此只依赖一张表，而不是散落各处的 `if tool == "shell"`。
   新增工具时忘了判权限是不可能的 —— 因为不声明 risk 就注册不上。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable


class Risk(str, Enum):
    """
    动作的风险等级。

    分级的依据是「错了之后能不能挽回」，不是「听起来危不危险」：
    - READ / SEARCH   改不了任何东西，错了无害
    - WRITE           改了本地文件，但可撤销（有 VCS / 有备份）
    - EXEC            跑命令，可能产生副作用，但通常可重来
    - EXEC_DESTRUCTIVE 删除、覆盖、强制推送一类，错了要花力气恢复
    - NETWORK         出网，可能泄露上下文，也可能产生外部副作用
    - IRREVERSIBLE    无法撤销（force-push 到公共分支、删库、发版）

    NETWORK 单独一档是因为它和"破坏"无关但和"不可逆"有关：
    数据一旦出去就收不回来。
    """

    READ = "read"
    SEARCH = "search"
    WRITE = "write"
    EXEC = "exec"
    EXEC_DESTRUCTIVE = "exec_destructive"
    NETWORK = "network"
    IRREVERSIBLE = "irreversible"


# 严重度序。用于"取两者中更严的那个"：一个工具声明的等级是下限，
# 具体动作的实际风险可能更高（shell 尤甚），需要往上抬而不是往下压。
RISK_ORDER: dict[Risk, int] = {
    Risk.READ: 0,
    Risk.SEARCH: 1,
    Risk.WRITE: 2,
    Risk.EXEC: 3,
    Risk.NETWORK: 4,
    Risk.EXEC_DESTRUCTIVE: 5,
    Risk.IRREVERSIBLE: 6,
}


# 状态取值。刻意不用 bool：
# 模型看到 `timeout` 会去改超时或换实现，看到 `awaiting_input` 会去换
# 非交互写法 —— 两者都是"没拿到结果"，但纠偏方向相反。
STATUS_OK = "ok"
STATUS_ERROR = "error"
STATUS_TIMEOUT = "timeout"
STATUS_AWAITING_INPUT = "awaiting_input"
STATUS_KILLED = "killed"
STATUS_DENIED = "denied"
STATUS_UNKNOWN_TOOL = "unknown_tool"
STATUS_BAD_ARGS = "bad_args"
STATUS_NEEDS_APPROVAL = "needs_approval"


@dataclass
class ToolResult:
    """工具执行结果。"""

    ok: bool
    output: str = ""
    error: str = ""
    status: str = STATUS_OK
    data: dict[str, Any] = field(default_factory=dict)
    truncated: bool = False
    elapsed_s: float = 0.0

    def to_observation(self) -> str:
        """
        渲染成喂给模型的一段文本。

        **失败时也必须带上已产出的部分输出。** 编译失败时前 200 行就是
        错误信息，超时被杀时前面已经吐出来的日志是唯一线索。只回一句
        `failed` 等于把纠偏材料丢掉，模型只能瞎猜重试。
        """
        parts: list[str] = []

        if self.ok:
            if self.output:
                parts.append(self.output)
            if self.truncated:
                parts.append("[输出已截断]")
            return "\n".join(parts)

        head = f"[{self.status}]"
        if self.error:
            head += f" {self.error}"
        parts.append(head)
        if self.output:
            parts.append("已产出的部分输出：")
            parts.append(self.output)
        if self.truncated:
            parts.append("[输出已截断]")
        return "\n".join(parts)

    def __str__(self) -> str:
        return self.to_observation()


@dataclass
class Tool:
    """一个工具。"""

    name: str
    description: str
    risk: Risk
    params: dict[str, Any]          # JSON Schema，供 LLM function calling
    fn: Callable[..., ToolResult]

    def to_schema(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "description": self.description,
            "risk": self.risk.value,
            "parameters": self.params,
        }


class ToolRegistry:
    """工具表。agent loop 与 LLM 都只看得见这一层。"""

    def __init__(self) -> None:
        self._tools: dict[str, Tool] = {}

    def register(self, tool: Tool) -> Tool:
        if tool.name in self._tools:
            raise ValueError(f"工具重名: {tool.name}")
        self._tools[tool.name] = tool
        return tool

    def get(self, name: str) -> Tool | None:
        return self._tools.get(name)

    def names(self) -> list[str]:
        return sorted(self._tools)

    def schema(self) -> list[dict[str, Any]]:
        """导出给 LLM 的工具表。"""
        return [self._tools[n].to_schema() for n in self.names()]

    def risk_of(self, name: str) -> Risk:
        t = self._tools.get(name)
        return t.risk if t else Risk.EXEC   # 未知工具按最保守处理

    def call(self, name: str, args: dict[str, Any] | None = None) -> ToolResult:
        """
        调用工具。任何错误都变成 ToolResult，不抛异常。

        未知工具返回 UNKNOWN_TOOL 而不是 KeyError：模型写错工具名是常态，
        它需要看到"没有这个工具，有这些"才能纠正自己。
        """
        args = args or {}
        tool = self._tools.get(name)
        if tool is None:
            return ToolResult(
                ok=False, status=STATUS_UNKNOWN_TOOL,
                error=f"没有名为 {name!r} 的工具。可用工具: {', '.join(self.names())}",
            )
        try:
            return tool.fn(**args)
        except TypeError as e:
            return ToolResult(ok=False, status=STATUS_BAD_ARGS,
                              error=f"{name} 参数不匹配: {e}")
        except Exception as e:  # noqa: BLE001 —— 工具内部异常必须降级为观测
            return ToolResult(ok=False, status=STATUS_ERROR,
                              error=f"{name} 执行异常: {type(e).__name__}: {e}")
