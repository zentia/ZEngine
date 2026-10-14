"""
MSVC + PDB 的符号化适配器与地址归一化。

**为什么单独写这个，而不是复用 Addr2LineResolver**

Addr2LineResolver 依赖 ELF/DWARF，对 MSVC 生成的 PDB 基本无用。
Windows 上要拿到「地址 -> 函数名 + 文件:行号」，可用的本地工具是
Visual Studio 自带的 `llvm-symbolizer`（VC/Tools/Llvm/x64/bin）。

**两个必须实测才能发现的坑**

1) ASLR 归一化。运行时崩溃地址带着随机基址（如 0x7ff77e4a720e），
   而 PDB 记录的是静态 preferred base（通常 0x140000000）。
   直接拿运行时地址去符号化**不会报错**，它会返回一个完全无关的函数名
   （实测返回 `__guard_memcpy_fptr`，行号 0）。
   这种假结果比直接失败危险得多 —— 它看起来是解析成功的。
   必须做：addr - runtime_base + static_base。

2) 能力探测。LLVM 某些版本对 PDB 支持很差，恒返回 `??:0`。
   所以不能假设「有工具就能符号化」，要拿已知地址实测，
   解析出有效行号才算具备能力，否则整体降级。

**相关的设计取舍**

行号为 0 的结果一律返回 None（而不是返回 Symbol 让上层自己判断）。
理由：把「未解析」和「解析成功但无行号」混在一起，
上层很容易拿它当结论用。宁可让流水线走「无法定位」路径。
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

from zagent.protocols import Revision, Symbol, SymbolResolver
from zagent.types import StackFrame


class MsSymbolResolver:
    """基于 LLVM symbolizer 的 Windows 符号化（含 ASLR 归一化）。"""

    # 本机装的是 Professional，其他版本也要覆盖，否则换台机器就失效。
    _SEARCH_ROOTS = [
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm",
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm",
        r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm",
        r"C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Tools\Llvm",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\Llvm",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\Llvm",
    ]

    # 判断地址是否落在自身模块内的保守上限。
    # 样本 exe 远小于 64MB，系统 DLL 与自身基址的差距远大于此。
    MODULE_SIZE_GUESS = 64 * 1024 * 1024

    def __init__(self, binary: Path, tool: Path | None = None,
                 static_base: int | None = None) -> None:
        self.binary = Path(binary)
        self.tool = Path(tool) if tool else self._find_tool()
        self._static_base = static_base
        self._capable: bool | None = None

    @classmethod
    def _find_tool(cls) -> Path | None:
        """先 PATH，再 VS 安装目录。"""
        found = shutil.which("llvm-symbolizer")
        if found:
            return Path(found)

        for root in cls._SEARCH_ROOTS:
            root_p = Path(root)
            if not root_p.exists():
                continue
            for arch in ("x64", "bin", "ARM64"):
                cand = root_p / arch / "llvm-symbolizer.exe"
                if cand.exists():
                    return cand
        return None

    @property
    def available(self) -> bool:
        return self.tool is not None and self.tool.exists() and self.binary.exists()

    # -------------------------------------------------- 地址归一化

    @staticmethod
    def parse_preferred_base(map_text: str) -> int | None:
        """从 .map 里取静态 preferred load address。"""
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", map_text)
        return int(m.group(1), 16) if m else None

    def static_base(self) -> int | None:
        if self._static_base is not None:
            return self._static_base
        mapf = self.binary.with_suffix(".map")
        if not mapf.exists():
            return None
        try:
            self._static_base = self.parse_preferred_base(
                mapf.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            return None
        return self._static_base

    def normalize(self, addr: int, runtime_base: int | None) -> int | None:
        """
        运行时地址 -> 静态地址。

        **这里有个真实的陷阱，值得单独说明。**

        崩溃指令常常不在我们自己的模块里。堆破坏（0xC0000374）是在
        ntdll 的堆管理器里被检测到的，崩溃 IP 落在系统 DLL 上；
        此时 runtime_base（我们模块的基址）与 addr 不属于同一模块，
        套用归一化公式会算出一个完全无意义的地址（实测得到 0x3728c7eb5）。

        这种错误特别危险：地址看起来是合法数字，符号化会返回一个
        毫不相干的函数名，比直接说「无法定位」糟糕得多。

        所以先判断 addr 是否落在 [runtime_base, runtime_base + size) 内，
        不在就不归一化并返回 None，让上层走「检测点不在项目代码」路径 ——
        这恰恰是真实崩溃分析里最重要的判断之一：
        **崩溃点是检测点，不是破坏点。**

        runtime_base 为 None 时同样返回 None：拿不到基址就不要假装归一化。
        """
        sb = self.static_base()
        if runtime_base is None or sb is None:
            return None

        # 是否落在自身模块内。size 未知时用 64MB 上限做保守判断 ——
        # 样本 exe 远小于此，而系统 DLL 与自身基址的差距远大于此。
        offset = addr - runtime_base
        if offset < 0 or offset > self.MODULE_SIZE_GUESS:
            return None

        return addr - runtime_base + sb

    # -------------------------------------------------- 能力探测

    def _run(self, *args: str) -> tuple[int, str]:
        if not self.available:
            return 1, ""
        try:
            p = subprocess.run([str(self.tool), *args],
                               capture_output=True, text=True,
                               timeout=30, errors="replace")
            return p.returncode, (p.stdout or "")
        except (subprocess.TimeoutExpired, OSError):
            return 1, ""

    def _first_text_address(self) -> int | None:
        """
        取一个已知代码段地址用于探测。

        只在有 .map 时才探测。没有 map 就返回 None 让能力降级 ——
        不用猜测的偏移做探测：猜出来的地址若碰巧解析成功，
        会掩盖「其实没能力」这个事实。
        """
        mapf = self.binary.with_suffix(".map")
        if not mapf.exists():
            return None
        try:
            text = mapf.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return None

        for line in text.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[-1].endswith(".obj"):
                try:
                    addr = int(parts[2], 16)
                except ValueError:
                    continue
                # 段选择符 0000: 是 guard 等特殊段，跳过
                if addr > 0x140000000 and not parts[0].startswith("0000:"):
                    return addr
        return None

    def ready(self) -> bool:
        if self._capable is not None:
            return self._capable
        self._capable = False

        if not self.available:
            return False

        probe = self._first_text_address()
        if probe is None:
            return False

        rc, out = self._run("-e", str(self.binary), "--functions", "--demangle",
                            "--output-style=GNU", hex(probe))
        if rc != 0 or not out.strip():
            return False

        # 必须解析出真实行号。??:0 视为不可用。
        m = re.search(r":(\d+)\s*$", out.strip())
        if not m or int(m.group(1)) == 0:
            return False

        self._capable = True
        return True

    # -------------------------------------------------- 主接口

    def resolve(self, frame: StackFrame, build: Revision) -> Symbol | None:
        if frame.address is None or not self.ready():
            return None

        # 优先用调用方已归一化好的地址；否则尝试自己归一化
        addr = frame.address
        rb = getattr(frame, "runtime_base", None)
        if rb:
            normalized = self.normalize(addr, rb)
            if normalized is None:
                # 地址不在本模块内（典型：堆破坏在系统 DLL 中被检测到）。
                # 返回 None 让上层走「检测点不在项目代码」路径，
                # 而不是拿一个无意义的地址去符号化。
                return None
            addr = normalized

        rc, out = self._run("-e", str(self.binary), "--functions", "--demangle",
                            "--output-style=GNU", hex(addr))
        if rc != 0 or not out.strip():
            return None

        lines = [x for x in out.splitlines() if x.strip()]
        if len(lines) < 2:
            return None

        func, loc = lines[0].strip(), lines[1].strip()
        m = re.match(r"^(.+?):(\d+)$", loc)
        if m:
            line = int(m.group(2))
            if line == 0:
                return None
            return Symbol(function=func, file=m.group(1), line=line,
                          module=frame.module, raw=loc)

        if func and func != "??":
            return Symbol(function=func, raw=loc, module=frame.module)
        return None


def build_resolver(binary: Path, **kw) -> MsSymbolResolver:
    return MsSymbolResolver(Path(binary), **kw)
