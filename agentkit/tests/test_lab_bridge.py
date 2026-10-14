"""lab_bridge 测试。

重点不是转换本身，而是**诚实性**：拿不到的东西必须留空并说明原因，
绝不能编一个看起来合理的值。合成数据测不出这类问题 ——
`frames: []` 在手工构造的样本里永远是"我故意留的空"。
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from zagent.lab_bridge import (
    capture_to_snapshot,
    lab_run_to_snapshot,
    write_snapshot,
)


def _cap(**kw) -> SimpleNamespace:
    """造一个最小的 CrashCapture 替身。

    用 SimpleNamespace 而不是真的 CrashCapture，是为了证明桥接层
    不依赖 zlab 的具体类型 —— 它有 Any 的契约，就该按 Any 测。
    """
    base = dict(
        exception_code=0xC0000005,
        crash_address=0x140001234,
        raw_address=0x7FF712A21234,
        frames=[0x1234, 0x1256],
        raw_frames=[0x7FF712A21234, 0x7FF712A21256],
        fault_address=None,
        uaf_alloc_ra=None,
        uaf_free_ra=None,
        doublefree_ra=None,
        hit_alloc_ra=None,
        race_conflict_ra=None,
        leak_alloc_ras=[],
        leak_count=0,
        notes=[],
    )
    base.update(kw)
    return SimpleNamespace(**base)


class TestHonesty(unittest.TestCase):
    """拿不到的东西不许编。"""

    def test_thread_id_never_faked_to_zero(self):
        """thread_id 未知时必须是 None，不能是 0。

        0 会被下游读成"主线程" —— 那是编造证据，比留空危险得多。
        """
        snap = capture_to_snapshot(_cap())
        self.assertIsNone(snap["thread_id"])

    def test_missing_build_id_gets_reason(self):
        """没有 build_id 就必须给出 reason。

        空串会被下游误读成"没抓到崩溃"，而真实原因是"这个构建没 PDB"。
        两者混淆会让 agent 在该换构建的时候去查崩溃路径。
        """
        snap = capture_to_snapshot(_cap())
        self.assertEqual(snap["build_id"], "")
        self.assertTrue(snap["build_id_reason"])

    def test_provided_reason_preserved(self):
        snap = capture_to_snapshot(_cap(), build_id_reason="no_pdb_release")
        self.assertEqual(snap["build_id_reason"], "no_pdb_release")

    def test_build_id_passed_through(self):
        snap = capture_to_snapshot(_cap(), build_id="abc123")
        self.assertEqual(snap["build_id"], "abc123")
        self.assertEqual(snap["build_id_reason"], "")


class TestStackReason(unittest.TestCase):
    """空栈必须区分"栈没用"和"没抓到栈"。

    uaf_01 是真实案例：它崩了（c0000005，地址已知），但崩溃点在探针
    代码里，栈指向探测器而非用户代码，所以探针不输出栈帧。这时空栈是
    **正确结果**。但如果只留空数组，下游无法和"采集失败"区分 ——
    前者该改用堆证据，后者该重跑。
    """

    def test_empty_stack_with_heap_evidence(self):
        cap = _cap(frames=[], raw_frames=[], uaf_alloc_ra=0x1100)
        snap = capture_to_snapshot(cap)
        self.assertEqual(snap["stack_reason"],
                         "stack_not_relevant_use_heap_evidence")

    def test_empty_stack_without_evidence(self):
        """既没栈也没堆证据 = 真出问题了，不能标成"设计如此"。"""
        cap = _cap(frames=[], raw_frames=[])
        snap = capture_to_snapshot(cap)
        self.assertEqual(snap["stack_reason"], "stack_unavailable")

    def test_nonempty_stack_has_no_reason(self):
        self.assertEqual(capture_to_snapshot(_cap())["stack_reason"], "")

    def test_leak_counts_as_evidence(self):
        cap = _cap(frames=[], raw_frames=[], leak_count=3)
        self.assertEqual(
            capture_to_snapshot(cap)["stack_reason"],
            "stack_not_relevant_use_heap_evidence")


class TestFrames(unittest.TestCase):
    def test_addresses_have_no_thousands_separator(self):
        """地址绝不能出现千分位 —— 和引擎侧 FormatHex 同理。"""
        cap = _cap(frames=[0x1234567890])
        snap = capture_to_snapshot(cap)
        self.assertNotIn(",", snap["frames"][0]["address"])
        self.assertEqual(snap["frames"][0]["address"], "1234567890")

    def test_raw_address_preserved(self):
        """原始地址必须保留：归一化会丢 ASLR 信息。"""
        snap = capture_to_snapshot(_cap(), module_base=0x7FF700000000)
        self.assertEqual(snap["frames"][0]["raw_address"], "7ff712a21234")
        self.assertEqual(snap["frames"][0]["module_base"], "7ff700000000")

    def test_frame_count_matches_source(self):
        snap = capture_to_snapshot(_cap())
        self.assertEqual(len(snap["frames"]), 2)


class TestWrite(unittest.TestCase):
    def test_roundtrip(self):
        with tempfile.TemporaryDirectory() as td:
            fp = write_snapshot(capture_to_snapshot(_cap()),
                                Path(td) / "crash_snap.json")
            data = json.loads(fp.read_text(encoding="utf-8"))
        self.assertEqual(data["schema"], "zengine-crash/1")
        self.assertEqual(data["exception_type"], "c0000005")

    def test_rejects_comma_pollution(self):
        """写出前拦住千分位污染，而不是让脏数据进流水线。"""
        bad = {"frames": [{"address": "1,234"}]}
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(ValueError):
                write_snapshot(bad, Path(td) / "crash_snap.json")


class TestLabRun(unittest.TestCase):
    def test_requires_capture(self):
        with self.assertRaises(ValueError):
            lab_run_to_snapshot(SimpleNamespace())

    def test_reads_capture_from_run(self):
        run = SimpleNamespace(capture=_cap())
        snap = lab_run_to_snapshot(run)
        self.assertEqual(snap["exception_type"], "c0000005")


if __name__ == "__main__":
    unittest.main()
