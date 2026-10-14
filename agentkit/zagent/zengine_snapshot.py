"""
ZEngine 真实崩溃快照消费端。

对接 engine/Source/Runtime/Core/Base/CrashHandler.h 产出的 crash_snap.json，
以及历史遗留的 crash_stack.txt（人读格式）。

为什么单独放一个模块而不是塞进 adapters.py：adapters 那一层刻意做得朴素，
是给外部 clone 的人看「框架怎么跑」的。真实工程的领域规则不该混进去。

关键防御：千分位污染。
引擎曾因全局 std::locale 把十六进制地址输出成 `0x7ff,9bb,33c,52f`。
历史 crash_stack.txt 全是这种格式。直接 int(x, 16) 会抛异常，
而更危险的是「看起来解析成功但地址是错的」。这里显式剥离逗号并留痕。
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from .types import Facts, StackFrame

# 带千分位逗号的十六进制，如 0x7ff,9bb,33c,52f
#
# 逗号分组必须是 + 而不是 *：
# 用 * 的话任何 `0x` 开头的普通 hex（0x7ff9bb33c52f）都会匹配上，
# 于是干净的新产物也被标成「污染」。假警报会淹没真信号，
# 让这个标记失去意义。必须至少出现一组逗号才算。
_COMMA_HEX = re.compile(r"0x[0-9a-fA-F]{1,3}(?:,[0-9a-fA-F]{3})+")

# 分步解析，而不是一个巨型正则。
#
# 帧格式（尾部三段都可选，顺序固定）：
#   [i] 0xADDR  FUNC +0xOFF  [MOD base=0xBASE]  (FILE:LINE)
#
# 一个巨型正则搞不定：所有尾部都是可选的，非贪婪的 func 分组会回溯到
# 只吃一个字符，而 +0x / [..] / (..) 全都「可匹配可不匹配」，引擎会挑
# 让整体成功但语义错的那条路（实测丢了 5 帧，且 func 吞掉行号）。
# 改成从右往左逐段剥离，每段都有明确锚点，行为可预测。

# 头部：[i] 0xADDR
_HEAD = re.compile(r"^\[(?P<idx>\d+)\]\s+0x(?P<addr>[0-9a-fA-F,]+)\s*(?P<rest>.*)$")

# 尾部 (FILE:LINE)。两处都必须容忍千分位逗号：
#   * file 贪婪到最后一个冒号，因为路径含盘符冒号 E:\...
#   * line 同样被污染过 —— 真实样本里出现过 `.cpp:2,393`，
#     写成 \d+ 会整段不匹配，进而让 func 吞掉整行（实测丢 5 帧）。
_TAIL_FILE = re.compile(r"\s+\((?P<file>.+):(?P<line>[0-9,]+)\)\s*$")

# 尾部 [MOD base=0xBASE]
_TAIL_MOD = re.compile(
    r"\s+\[(?P<mod>[^\s\]]+)(?:\s+base=0x(?P<base>[0-9a-fA-F,]+))?\]\s*$"
)

# 尾部 +0xOFF
_TAIL_OFF = re.compile(r"\s+\+\s*0x(?P<off>[0-9a-fA-F,]+)\s*$")


def parse_hex(value: str) -> int | None:
    """
    解析十六进制地址，容忍千分位逗号。

    返回 None 而不是抛异常：崩溃现场的解析不该因为一帧坏掉就整体失败，
    但也不能静默给个错的。None 会被上层记入 Facts.missing。
    """
    if value is None:
        return None
    s = str(value).strip()
    if not s:
        return None
    if s.lower().startswith("0x"):
        s = s[2:]
    s = s.replace(",", "")
    if not s:
        return None
    try:
        return int(s, 16)
    except ValueError:
        return None


def has_comma_pollution(text: str) -> bool:
    """文本里是否存在千分位污染的十六进制地址。"""
    return bool(_COMMA_HEX.search(text or ""))


def load_json_snapshot(path: Path, ref: str = "") -> Facts:
    """读 crash_snap.json（schema zengine-crash/1）。"""
    path = Path(path)
    data = json.loads(path.read_text(encoding="utf-8"))

    if data.get("schema") != "zengine-crash/1":
        raise ValueError(f"未知快照 schema: {data.get('schema')!r}")

    frames: list[StackFrame] = []
    for f in data.get("frames", []):
        addr = parse_hex(f.get("address"))
        base = parse_hex(f.get("module_base"))
        frames.append(
            StackFrame(
                module=f.get("module", "") or "",
                address=addr,
                function=f.get("function", "") or "",
                file=f.get("file", "") or "",
                line=f.get("line") or None,
            )
        )

    facts = Facts(
        ref=ref or str(path),
        platform=data.get("platform", ""),
        product_version=data.get("product_version", ""),
        build_id=data.get("build_id", ""),
        build_id_reason=data.get("build_id_reason", ""),
        build_timestamp=int(data.get("build_timestamp") or 0),
        exception_type=data.get("exception_type", ""),
        exception_message=data.get("exception_message", ""),
        stack=frames,
        crash_time=data.get("crash_time", ""),
        uid=data.get("uid", ""),
        device=data.get("device", ""),
    )

    # 缺字段显式记录，绝不用默认值蒙混
    for name in ("platform", "product_version", "exception_type", "crash_time"):
        if not getattr(facts, name):
            facts.note_missing(name)
    if not facts.stack:
        facts.note_missing("stack")

    # build_id 拿不到时必须留痕。Release 无 PDB 是最常见的情形，
    # 这时后面所有依赖版本的推断都要降级 —— 静默留空等于骗人。
    if not facts.build_id:
        facts.note_missing("build_id")
        if facts.build_id_reason:
            facts.note_missing("build_id_reason:" + facts.build_id_reason)

    return facts


def parse_dec(value: str) -> int | None:
    """
    解析十进制行号，同样容忍千分位逗号（`2,393` -> 2393）。

    行号被污染这点很容易漏：地址带逗号一眼能看出来，
    但 `.cpp:2,393` 混在路径里，看起来就像路径的一部分。
    """
    if value is None:
        return None
    s = str(value).strip().replace(",", "")
    if not s:
        return None
    try:
        return int(s, 10)
    except ValueError:
        return None


def _parse_frame_line(line: str) -> StackFrame | None:
    """
    按 _HEAD / _TAIL_* 分步解析一帧。任一段拿不到就留空，不整帧丢弃。

    地址是唯一的硬要求：拿不到地址这帧就没有价值（无法符号化、无法算 RVA），
    所以返回 None。其余字段缺失都允许——Release 无 PDB 时本来就没有行号。
    """
    m = _HEAD.match(line.strip())
    if not m:
        return None

    addr = parse_hex(m.group("addr"))
    if addr is None:
        return None

    rest = m.group("rest") or ""
    module = ""
    file = ""
    line_no = None

    # 从右往左剥：先 (FILE:LINE)，再 [MOD base=..]，再 +0xOFF
    mf = _TAIL_FILE.search(rest)
    if mf:
        file = (mf.group("file") or "").strip()
        line_no = parse_dec(mf.group("line"))
        rest = rest[: mf.start()]

    mm = _TAIL_MOD.search(rest)
    if mm:
        module = (mm.group("mod") or "").strip()
        rest = rest[: mm.start()]

    mo = _TAIL_OFF.search(rest)
    if mo:
        rest = rest[: mo.start()]

    # 剩下的就是函数名。可能是 <no symbol>
    func = rest.strip()
    if func == "<no symbol>":
        func = ""

    return StackFrame(
        module=module,
        address=addr,
        function=func,
        file=file,
        line=line_no,
    )


def load_text_stack(path: Path, ref: str = "") -> Facts:
    """
    读历史 crash_stack.txt（人读格式）。

    这个格式没有 schema 保障，行与行之间可能缺字段（Release 无 PDB 时
    退化成 `ZEngineApi +0x172,250`）。解析失败的一帧保持 file/line 为空，
    但地址必须拿到——地址是后续符号化的唯一抓手。
    """
    path = Path(path)
    text = path.read_text(encoding="utf-8", errors="replace")

    polluted = has_comma_pollution(text)

    frames: list[StackFrame] = []
    exception_type = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue

        if line.startswith("ZEngine crash") or line.startswith("ZEditor crash"):
            m = re.search(r"ExceptionCode=0x([0-9a-fA-F,]+)", line)
            if m:
                v = parse_hex(m.group(1))
                if v is not None:
                    exception_type = f"0x{v:08x}"
            continue

        frame = _parse_frame_line(line)
        if frame is None:
            continue
        frames.append(frame)

    facts = Facts(
        ref=ref or str(path),
        platform="windows",
        exception_type=exception_type,
        stack=frames,
    )

    if polluted:
        # 不静默：这是已知的历史污染，显式标记出来，
        # 免得后面有人拿逗号地址去算 RVA 还以为是对的。
        facts.note_missing("comma_polluted_hex_stripped")
    if not exception_type:
        facts.note_missing("exception_type")
    if not frames:
        facts.note_missing("stack")

    return facts


def load_snapshot(path: Path, ref: str = "") -> Facts:
    """按扩展名分派。.json 走 schema 路径，其余按人读文本解析。"""
    path = Path(path)
    if path.suffix.lower() == ".json":
        return load_json_snapshot(path, ref)
    return load_text_stack(path, ref)
