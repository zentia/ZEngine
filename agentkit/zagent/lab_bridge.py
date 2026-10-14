"""把 zlab 的真实崩溃捕获转成 zengine-crash/1 快照。

为什么需要这一层
----------------
L1 做了 crash_snap.json 的解析器，但它的验证全靠手工构造 JSON —— 那是
**自己出题自己答**。zlab 那边有 8 个真实编译、真实崩溃、真实符号化的样本
（CrashCapture 里是货真价实的 NTSTATUS 和栈帧），却没有任何路径把它变成
快照喂给解析器。

于是出现一个危险的空洞：解析器从未在真实崩溃上跑过，而 eval 报出的
top1/top3 = 100% 全部来自合成样本 —— 这个数字什么都不能证明。

本模块就是那座桥。它的价值不在转换本身，而在于**让合成样本的 100% 第一次
有机会被真实数据证伪**。

诚实性约束
----------
CrashCapture 里没有的字段一律留空并记下原因，**绝不编造**：

- 没有 file/line -> 留空。符号化失败就是失败，不猜"大概在第几行"。
- 没有 build_id -> 留空 + 记 reason。lab 的可执行文件大多没有 PDB，
  这正是真实世界的常态，不能假装拿到了。
- thread_id 未知 -> 留空，绝不填 0。填 0 会被下游当成"主线程"，那是编造。
- 栈为空 -> 必须说明是"栈对本案无用"还是"没抓到栈"。这两者混淆会让
  agent 在该停的时候反复重试，或该重试的时候直接放弃。
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

SCHEMA = "zengine-crash/1"

# 有这些运行时证据时，空栈是**设计如此**而非采集失败：
# 崩溃点在探针代码里，栈指向探测器而不是用户代码，堆证据才是定位依据。
_HEAP_EVIDENCE_FIELDS = (
    "uaf_alloc_ra",
    "uaf_free_ra",
    "doublefree_ra",
    "hit_alloc_ra",
    "race_conflict_ra",
)


def _hex(value: int | None) -> str:
    """十六进制，绝不能带千分位分隔符。"""
    if value is None:
        return ""
    return format(value, "x")


def capture_to_snapshot(
    capture: Any,
    *,
    module: str = "",
    module_base: int | None = None,
    product_version: str = "",
    build_id: str = "",
    build_id_reason: str = "",
) -> dict:
    """把 CrashCapture 转成 crash_snap.json 的 dict。

    capture 声明为 Any 而不是具体类型，是为了不让 zlab 成为 zagent 的硬依赖：
    任何带 frames / exception_code 的对象都能用。
    """
    frames = []
    raw_frames = list(getattr(capture, "raw_frames", None) or [])
    normalized = list(getattr(capture, "frames", None) or [])

    # 归一化帧是相对 module_base 的偏移，原始帧是运行时真实地址。
    # 两者长度不一致时以归一化为准，原始帧按位置对齐，缺则留空。
    for i, pc in enumerate(normalized):
        raw = raw_frames[i] if i < len(raw_frames) else None
        frames.append({
            "index": i,
            "module": module,
            "address": _hex(pc),
            "module_base": _hex(module_base),
            # 原始地址单独保留：归一化会丢掉 ASLR 信息，而排查
            # "这次崩溃和上次的基址是否一致"必须靠它。
            "raw_address": _hex(raw),
            "function": "",
            "offset": "",
            "file": "",
            "line": 0,
        })

    # 空栈必须说明原因。uaf_01 就是典型：它确实崩了（c0000005，崩溃地址
    # 已知），但崩溃点落在探针代码里，栈指向探测器而不是用户代码，
    # 探针因此不输出栈帧。这时 frames=[] 是**正确结果**，但如果只留空数组，
    # 下游无法区分"栈对本案没用"和"我没抓到栈"。
    stack_reason = ""
    if not normalized:
        has_heap_evidence = any(
            getattr(capture, n, None) for n in _HEAP_EVIDENCE_FIELDS
        ) or bool(getattr(capture, "leak_count", 0))
        stack_reason = (
            "stack_not_relevant_use_heap_evidence" if has_heap_evidence
            else "stack_unavailable"
        )

    reason_out = build_id_reason
    if not build_id and not reason_out:
        # 拿不到 build_id 又没给理由 —— 这是不可接受的状态。
        # 空串会被下游误读成"没抓到崩溃"，而真实原因是"这个构建没 PDB"。
        reason_out = "not_provided_by_capture_source"

    return {
        "schema": SCHEMA,
        "platform": "windows",
        "exception_type": _hex(getattr(capture, "exception_code", None)),
        "exception_address": _hex(getattr(capture, "crash_address", None)),
        # thread_id：CrashCapture 没有这个字段就留空。
        # 绝不填 0 —— 那会被下游当成"主线程"，属于编造证据。
        "thread_id": None,
        "product_version": product_version,
        "build_id": build_id,
        "build_timestamp": 0,
        "build_id_reason": reason_out,
        "stack_reason": stack_reason,
        "frames": frames,
        # 保留探针证据：这些是运行时真实观测，比静态猜断可靠得多
        "evidence": {
            "uaf_alloc_ra": _hex(getattr(capture, "uaf_alloc_ra", None)),
            "uaf_free_ra": _hex(getattr(capture, "uaf_free_ra", None)),
            "doublefree_ra": _hex(getattr(capture, "doublefree_ra", None)),
            "leak_alloc_ras": [
                _hex(x) for x in (getattr(capture, "leak_alloc_ras", None) or [])
            ],
            "leak_count": getattr(capture, "leak_count", 0),
            "race_conflict_ra": _hex(getattr(capture, "race_conflict_ra", None)),
            "hit_alloc_ra": _hex(getattr(capture, "hit_alloc_ra", None)),
            "fault_address": _hex(getattr(capture, "fault_address", None)),
        },
        "notes": list(getattr(capture, "notes", None) or []),
    }


def write_snapshot(data: dict, path: Path) -> Path:
    """落盘，并防御千分位污染。"""
    text = json.dumps(data, indent=2, ensure_ascii=False)
    # 数字字段不该出现逗号分隔符。这条检查和引擎侧 FormatDec 同理：
    # 千分位污染是静默的，扫一眼 JSON 未必发现得了。
    if re.search(r'"[^"]*\d,\d', text):
        raise ValueError("疑似千分位污染，拒绝写出")
    path.write_text(text, encoding="utf-8")
    return path


def lab_run_to_snapshot(run: Any, **kwargs) -> dict:
    """从 LabRun 取 capture 并转换。"""
    capture = getattr(run, "capture", None)
    if capture is None:
        raise ValueError("LabRun 没有 capture —— 这不是一次崩溃运行")
    return capture_to_snapshot(capture, **kwargs)
