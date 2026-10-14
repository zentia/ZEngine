"""
参考适配器实现。全部基于本地可获得的工具，无内网依赖。

这一层刻意做得朴素：目的是证明「框架能跑」，而不是「实现有多强」。
真要用在真实工程上，把这些换掉即可，接口不变。
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

from .protocols import (ArtifactStore, CheckoutResult, CodeIndex, CrashSource,
                        Revision, Symbol, SymbolResolver, VersionResolver)
from .types import Facts, SourceLocation, StackFrame


# ============================================================ 版本

class SimpleVersionResolver:
    """
    版本解析器。

    真实工程里规则往往是「母包版本末段即 SVN revision」这类约定，
    这里给一个可覆盖的默认实现：优先取末段数字。
    """

    def __init__(self, prefix: str = "r") -> None:
        self.prefix = prefix

    def resolve(self, product_version: str) -> Revision | None:
        """
        产品版本 -> revision。

        默认规则：取末段纯数字（对应「母包版本末段即 revision」这类约定）。
        若完全没有数字段，则回退用整个版本串——例如直接传 git sha 的场景。
        """
        if not product_version:
            return None
        parts = [p for p in re.split(r"[.\-_]", product_version) if p.isdigit()]
        if parts:
            return Revision(id=f"{self.prefix}{parts[-1]}", note=f"from {product_version}")
        return Revision(id=product_version, note="raw")


def find_git() -> str | None:
    """找到可用的 git 可执行文件，找不到返回 None。

    **为什么需要这个函数**

    git 装在 C:\\Program Files\\Git\\cmd 却没加进 PATH 是很常见的情形。
    直接 `shutil.which("git")` 会返回 None，于是各处各写各的降级：
    adapters.py 回退成字符串 "git not found"，examples/run_eval.py
    回退成 "nogit"。两份降级互不知情，版本对齐门禁拿两个垃圾值去比对，
    结果取决于它们恰好是否相等 —— 门禁等于没生效。

    git 探测只该有一份实现，且失败必须表现为"没有"而不是"某个字符串"。
    """
    found = shutil.which("git")
    if found:
        return found
    for c in (r"C:\Program Files\Git\cmd\git.exe",
              r"C:\Program Files\Git\bin\git.exe",
              r"C:\Program Files (x86)\Git\cmd\git.exe"):
        if Path(c).exists():
            return c
    return None


# ============================================================ 代码索引

class GitCodeIndex:
    """
    基于 git 的代码索引。

    安全约束（对应「版本对齐门禁」）：
    - checkout 前先检查工作区是否 dirty，dirty 且不允许则直接失败
    - 冲突不自动解决，原样上报
    - 绝不执行 reset --hard / clean -fd
    """

    def __init__(self, root: Path, project_globs: tuple[str, ...] = ("engine/", "src/")) -> None:
        self.root = Path(root)
        self.project_globs = project_globs

    # git 不在 PATH 时的常见位置。git 装在 C:\Program Files\Git\cmd 却没
    # 加进 PATH 是很常见的情形，直接判"没有 git"会让版本对齐门禁失去依据。
    def _git(self, *args: str) -> tuple[int, str]:
        exe = find_git()
        if exe is None:
            return 1, ""
        try:
            p = subprocess.run([exe, *args], cwd=self.root,
                               capture_output=True, text=True, timeout=60)
            return p.returncode, (p.stdout or "").strip()
        except subprocess.TimeoutExpired:
            return 1, ""
        except (OSError, subprocess.SubprocessError):
            return 1, ""

    def current_revision(self) -> Revision:
        """
        当前 revision。**失败时必须返回空 id，绝不能把错误文本当版本号。**

        这里曾经写 `Revision(id=out or "unknown")`，忽略了返回码。
        于是 git 不可用时，"git not found" 这个字符串被当成了一个 revision，
        版本对齐门禁拿它去和崩溃端的 "nogit" 比对 —— 结果当然是"不一致"，
        4 个合成样本全部被 block，top1 掉到 0%。

        教训：**错误信息不是数据**。拿不到就说拿不到（空 id + note 说明），
        让门禁走"版本无法确定"的分支；把错误文本塞进数据字段，
        只会让它看起来像是有值，从而绕过所有缺失检查。
        """
        rc, out = self._git("rev-parse", "HEAD")
        if rc != 0 or not out:
            return Revision(id="", note="revision unavailable (git failed)")
        return Revision(id=out, note="HEAD")

    def is_dirty(self) -> bool:
        _, out = self._git("status", "--porcelain")
        return bool(out.strip())

    def checkout(self, rev: Revision, allow_dirty: bool = True) -> CheckoutResult:
        if self.is_dirty() and not allow_dirty:
            return CheckoutResult(False, conflict=True,
                                  message="工作区有未提交改动，拒绝切换（不自动 revert）")

        rc, out = self._git("checkout", rev.id)
        if rc != 0:
            # 冲突不明文处理，上报给人
            rc2, conflicts = self._git("diff", "--name-only", "--diff-filter=U")
            return CheckoutResult(
                False, conflict=True,
                conflicts=[x for x in conflicts.splitlines() if x.strip()],
                message=out or "checkout failed",
            )
        return CheckoutResult(True, message=f"switched to {rev.id}")

    def is_project_code(self, frame: StackFrame) -> bool:
        if not frame.file and not frame.function:
            return False
        target = frame.file or frame.function
        return any(target.startswith(g) or g in target for g in self.project_globs)

    def map_to_local(self, symbol: Symbol) -> SourceLocation | None:
        """
        符号 -> 本地源码。

        真实工程里这一步往往是「转译函数名还原原始类方法」（如 il2cpp 命名规则），
        这里只做直接路径匹配。映射不了返回 None —— 不猜是关键。
        """
        if not symbol.file:
            return None
        candidate = self.root / symbol.file
        if not candidate.exists():
            # 退一步：按文件名在工作区内搜
            hits = self.grep(re.escape(Path(symbol.file).name), limit=1)
            if not hits:
                return None
            candidate = hits[0][0]
        line = symbol.line or 1
        return SourceLocation(path=candidate, line=line, symbol=symbol.function)

    def read_lines(self, path: Path) -> list[str]:
        try:
            return path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return []

    def read_context(self, loc: SourceLocation, radius: int = 50) -> str:
        try:
            lines = loc.path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return ""
        lo = max(0, loc.line - 1 - radius)
        hi = min(len(lines), loc.line + radius)
        return "\n".join(f"{i+1}: {lines[i]}" for i in range(lo, hi))

    def grep(self, pattern: str, limit: int = 200) -> list[tuple[Path, int, str]]:
        """
        轻量 grep。优先用 ripgrep，没有则退回 Python 遍历。

        只用 Python 标准库，不引入额外依赖，保证任何人 clone 下来就能跑。
        """
        rg = shutil.which("rg")
        out: list[tuple[Path, int, str]] = []

        if rg:
            try:
                p = subprocess.run(
                    [rg, "-n", "--no-heading", "--color", "never", pattern,
                     str(self.root)],
                    capture_output=True, text=True, timeout=60,
                )
                for line in (p.stdout or "").splitlines()[:limit]:
                    m = re.match(r"^(.+?):(\d+):(.*)$", line)
                    if m:
                        out.append((Path(m.group(1)), int(m.group(2)), m.group(3)))
                return out
            except (subprocess.TimeoutExpired, OSError):
                pass

        # 回退：Python 遍历（限文本文件）
        try:
            rx = re.compile(pattern)
        except re.error:
            return []
        suffixes = {".cpp", ".h", ".hpp", ".c", ".cs", ".lua", ".py", ".ts", ".js"}
        for f in self.root.rglob("*"):
            if len(out) >= limit:
                break
            if not f.is_file() or f.suffix not in suffixes:
                continue
            if any(part in {".git", "build", "node_modules"} for part in f.parts):
                continue
            try:
                for i, line in enumerate(f.read_text(encoding="utf-8",
                                                     errors="replace").splitlines(), 1):
                    if rx.search(line):
                        out.append((f, i, line))
                        break
            except OSError:
                continue
        return out


# ============================================================ 崩溃数据源

class LocalDumpSource:
    """
    本地崩溃数据源。

    读一个 JSON 描述的崩溃样本（由故障注入器或手工生成）。
    这样设计的理由：让整个流水线在没有真实 dump、没有内网、没有符号服务器时也能跑通，
    从而可以被外部的人 clone 下来验证。
    """

    def __init__(self, sample_path: Path) -> None:
        self.sample_path = Path(sample_path)
        self.name = "local_dump"

    def ready(self) -> bool:
        return self.sample_path.exists()

    def fetch(self, ref: str) -> Facts:
        import json

        data = json.loads(self.sample_path.read_text(encoding="utf-8"))
        frames = [
            StackFrame(
                module=f.get("module", ""),
                address=f.get("address"),
                function=f.get("function", ""),
                file=f.get("file", ""),
                line=f.get("line"),
            )
            for f in data.get("stack", [])
        ]

        facts = Facts(
            ref=ref or str(self.sample_path),
            platform=data.get("platform", ""),
            product_version=data.get("product_version", ""),
            exception_type=data.get("exception_type", ""),
            exception_message=data.get("exception_message", ""),
            stack=frames,
            crash_time=data.get("crash_time", ""),
            uid=data.get("uid", ""),
            device=data.get("device", ""),
            uptime_seconds=data.get("uptime_seconds"),
        )

        # 缺字段显式记录，绝不用默认值蒙混
        for name in ("platform", "product_version", "exception_type", "crash_time", "uid"):
            if not getattr(facts, name):
                facts.note_missing(name)
        if not facts.stack:
            facts.note_missing("stack")

        return facts


# ============================================================ 产物与符号

class LocalArtifactStore:
    """本地产物目录。真实场景换成 CI artifacts / 符号服务器即可。"""

    def __init__(self, base: Path) -> None:
        self.base = Path(base)

    def fetch(self, build: Revision, kind: str) -> Path | None:
        p = self.base / build.id / kind
        return p if p.exists() else None

    def mount(self, artifact: Path, into: Path) -> list[Path]:
        # 真实场景这里可能是解压；本地目录就直接列出
        if artifact.is_dir():
            return sorted(artifact.rglob("*"))[:1000]
        return [artifact]


class Addr2LineResolver:
    """addr2line 符号化。找不到工具就返回 None，由流水线降级而不是崩掉。"""

    def __init__(self, binary: Path, tool: str = "llvm-addr2line") -> None:
        self.binary = Path(binary)
        self.tool = tool

    def resolve(self, frame: StackFrame, build: Revision) -> Symbol | None:
        if frame.address is None or not self.binary.exists():
            return None
        exe = shutil.which(self.tool) or shutil.which("addr2line")
        if not exe:
            return None
        try:
            p = subprocess.run(
                [exe, "-f", "-C", "-e", str(self.binary), hex(frame.address)],
                capture_output=True, text=True, timeout=30,
            )
            lines = (p.stdout or "").splitlines()
            if len(lines) < 2:
                return None
            func, loc = lines[0].strip(), lines[1].strip()
            m = re.match(r"^(.+?):(\d+)$", loc)
            if m:
                return Symbol(function=func, file=m.group(1), line=int(m.group(2)),
                              module=frame.module)
            return Symbol(function=func, raw=loc, module=frame.module)
        except (subprocess.TimeoutExpired, OSError):
            return None
