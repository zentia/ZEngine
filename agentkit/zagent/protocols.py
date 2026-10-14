"""
可替换适配器接口（Protocol）。

这一层决定了「能不能脱离某个具体工程复现」。
除 Verifier 外全是纯 IO，换实现不难；Verifier 依赖领域知识，是护城河所在。

参考实现见 adapters.py：git 代码索引、本地 dump、addr2line 符号化。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, runtime_checkable

from .types import Facts, Hypothesis, Revision, SourceLocation, StackFrame


# ---------------------------------------------------------------- 结果类型

@dataclass
class CheckoutResult:
    ok: bool = False
    conflict: bool = False
    conflicts: list[str] = None  # type: ignore[assignment]
    message: str = ""

    def __post_init__(self) -> None:
        if self.conflicts is None:
            self.conflicts = []


@dataclass
class Symbol:
    function: str = ""
    file: str = ""
    line: int | None = None
    module: str = ""
    raw: str = ""


# ---------------------------------------------------------------- 数据源

@runtime_checkable
class CrashSource(Protocol):
    """崩溃数据源。参考实现：LocalDumpSource（读合成 JSON / minidump 元数据）。"""

    name: str

    def ready(self) -> bool:
        """能力自检。不可用必须返回 False，由流水线 fail-fast。"""
        ...

    def fetch(self, ref: str) -> Facts:
        """取崩溃事实。取不到的字段必须 note_missing，不允许填默认值蒙混。"""
        ...


# ---------------------------------------------------------------- 版本

@runtime_checkable
class VersionResolver(Protocol):
    """产品版本 -> 源码 revision。这是「版本对齐门禁」的核心。"""

    def resolve(self, product_version: str) -> Revision | None:
        ...


@runtime_checkable
class CodeIndex(Protocol):
    """代码读写与检索。参考实现：GitCodeIndex（git worktree + grep）。"""

    def current_revision(self) -> Revision:
        ...

    def checkout(self, rev: Revision, allow_dirty: bool = True) -> CheckoutResult:
        """
        切到目标版本。

        硬约束：
        - 不允许 revert / stash / 丢弃本地改动
        - 冲突必须上报，不允许自动选远端版本
        - 不允许静默换分支
        """
        ...

    def is_project_code(self, frame: StackFrame) -> bool:
        """判断栈帧是否属于项目代码（排除运行时/引擎/系统帧）。"""
        ...

    def map_to_local(self, symbol: Symbol) -> SourceLocation | None:
        """转译代码 / 生成代码 -> 原始源码位置。映射不了返回 None，不猜。"""
        ...

    def read_context(self, loc: SourceLocation, radius: int = 50) -> str:
        ...

    def read_lines(self, path: Path) -> list[str]:
        """
        读整个文件的行（不含行号前缀）。

        单独提供这个接口，是为了让「函数体界定」这类需要跨行扫描的逻辑
        能通过适配器注入，而不是实现里直接 open 文件——
        否则测试无法用内存桩替换，也无法适配非文件系统来源。
        """
        ...

    def grep(self, pattern: str) -> list[tuple[Path, int, str]]:
        """返回 [(文件, 行号, 行内容)]。供验证规则使用。"""
        ...


# ---------------------------------------------------------------- 产物与符号

@runtime_checkable
class ArtifactStore(Protocol):
    """构建产物（真机代码 / 符号表）。参考实现：LocalArtifactStore（本地目录）。"""

    def fetch(self, build: Revision, kind: str) -> Path | None:
        ...

    def mount(self, artifact: Path, into: Path) -> list[Path]:
        ...


@runtime_checkable
class SymbolResolver(Protocol):
    """地址 -> 符号。参考实现：Addr2LineResolver（llvm-addr2line / atos）。"""

    def resolve(self, frame: StackFrame, build: Revision) -> Symbol | None:
        ...


# ---------------------------------------------------------------- 推理与验证

@runtime_checkable
class Reasoner(Protocol):
    """
    假设生成器。由 LLM 或规则实现。

    关键约束：产出的 claim 必须可证伪。
    「可能是内存问题」不可证伪，不该产出；
    「崩溃点在分配器而非破坏点」可证伪，才是合格的假设。
    """

    def propose(self, facts: Facts, locations: list[SourceLocation]) -> list[Hypothesis]:
        ...


@runtime_checkable
class Verifier(Protocol):
    """
    假设验证器。唯一难以替换的部分——它依赖领域知识。

    每条假设必须给出 (Verdict, Evidence)，Evidence 的 locator 必须可回溯。
    """

    def check(self, h: Hypothesis, code: CodeIndex) -> tuple[str, object]:
        ...


# ---------------------------------------------------------------- 注册表

@dataclass
class Context:
    """执行上下文。装配所有适配器。"""

    source: CrashSource
    version: VersionResolver
    code: CodeIndex
    artifacts: ArtifactStore | None = None
    symbols: SymbolResolver | None = None
    reasoner: Reasoner | None = None
    verifier: Verifier | None = None

    def ready_check(self) -> list[str]:
        """返回缺失的能力名。空列表表示全部就绪。"""
        missing = []
        if not self.source.ready():
            missing.append("source")
        for name in ("version", "code"):
            if getattr(self, name) is None:
                missing.append(name)
        return missing
