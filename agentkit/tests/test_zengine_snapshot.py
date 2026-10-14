"""
ZEngine 真实崩溃快照解析的回归测试。

样本不是手造的理想串，而是直接抄自工程里真实出现过的 crash_stack.txt。
这一点很重要：千分位污染（0x7ff,9bb,33c,52f / .cpp:2,393）是真实踩过的坑，
只用干净的 `0x7ff9bb33c52f` 写测试等于没测。
"""

from __future__ import annotations

import unittest
from pathlib import Path

from zagent.zengine_snapshot import (has_comma_pollution, load_snapshot,
                                     parse_dec, parse_hex)

# 真实样本片段（E:\Engine\ZEngine\crash_stack.txt 的前几帧）
# 注意：地址、偏移量、行号全都带千分位逗号。
REAL_SAMPLE = """ZEngine crash. ExceptionCode=0xc0,000,005 at 0,000,7FF,9BB,33C,52F tid=56,792

[0] 0x7ff,9bb,33c,52f  D3D12Descriptor::GetHeapResourceId +0x1f  (E:\\Engine\\ZEngine\\tools\\renderdoc\\renderdoc\\driver\\d3d12\\d3d12_manager.cpp:592)
[1] 0x7ff,9bb,0fe,b0f  WrappedID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable +0x19f  (E:\\Engine\\ZEngine\\tools\\renderdoc\\renderdoc\\driver\\d3d12\\d3d12_command_list_wrap.cpp:2,393)
[2] 0x7ff,748,080,a52  DX12RHI::CmdBindDescriptorSetsPFN +0x552  (E:\\Engine\\ZEngine\\engine\\Source\\Runtime\\Function\\Render\\Interface\\DX12\\DX12RHI.cpp:4,549)
[15] 0x7ff,b33,c6e,957  BaseThreadInitThunk +0x17
"""

# Release 无 PDB 的退化形态
RELEASE_SAMPLE = """ZEngine crash. ExceptionCode=0xc0,000,005 at 0,000,7FF,7D1,936,E10 tid=84,268

[0] 0x7ff,7d1,936,e10  ZEngineApi +0x172,250
[1] 0x7ff,7d1,8ce,74a  ZEngineApi +0x109,b8a
"""


class TestParseHex(unittest.TestCase):
    """千分位逗号的剥离。"""

    def test_strips_commas(self):
        self.assertEqual(parse_hex("0x7ff,9bb,33c,52f"), 0x7FF9BB33C52F)

    def test_plain_hex_still_works(self):
        self.assertEqual(parse_hex("0x7ff9bb33c52f"), 0x7FF9BB33C52F)

    def test_without_prefix(self):
        self.assertEqual(parse_hex("deadbeef"), 0xDEADBEEF)

    def test_zero(self):
        self.assertEqual(parse_hex("0x0"), 0)

    def test_garbage_returns_none_not_exception(self):
        """坏输入返回 None，不能抛异常把整条流水线打断。"""
        self.assertIsNone(parse_hex("not-hex"))
        self.assertIsNone(parse_hex(""))
        self.assertIsNone(parse_hex(None))


class TestParseDec(unittest.TestCase):
    def test_strips_commas(self):
        self.assertEqual(parse_dec("2,393"), 2393)

    def test_plain(self):
        self.assertEqual(parse_dec("592"), 592)

    def test_garbage(self):
        self.assertIsNone(parse_dec("abc"))


class TestCommaDetection(unittest.TestCase):
    def test_detects_pollution(self):
        self.assertTrue(has_comma_pollution(REAL_SAMPLE))

    def test_clean_text_not_flagged(self):
        self.assertFalse(has_comma_pollution("[0] 0x7ff9bb33c52f foo +0x1f"))


class TestRealSampleParsing(unittest.TestCase):
    """拿真实污染样本跑解析。"""

    def _write(self, tmp: Path, text: str) -> Path:
        tmp.mkdir(parents=True, exist_ok=True)
        p = tmp / "crash_stack.txt"
        p.write_text(text, encoding="utf-8")
        return p

    def test_all_frames_recovered(self):
        """17 帧里最容易丢的是 func 后跟 +0x 又跟 (path:line) 的那些。

        曾经因为行号带逗号（`2,393`）导致 _TAIL_FILE 不匹配，
        进而让 func 吞掉整行，一丢就是 5 帧。
        """
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            facts = load_snapshot(self._write(Path(td), REAL_SAMPLE))

        self.assertEqual(len(facts.stack), 4)
        self.assertEqual(facts.exception_type, "0xc0000005")

        f0 = facts.stack[0]
        self.assertEqual(f0.address, 0x7FF9BB33C52F)
        self.assertEqual(f0.function, "D3D12Descriptor::GetHeapResourceId")
        self.assertEqual(f0.line, 592)
        self.assertTrue(f0.file.endswith("d3d12_manager.cpp"))

    def test_line_number_comma_stripped(self):
        """行号 2,393 必须还原成 2393，不能被当成路径的一部分。"""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            facts = load_snapshot(self._write(Path(td), REAL_SAMPLE))

        f1 = facts.stack[1]
        self.assertEqual(f1.line, 2393)
        self.assertEqual(f1.function,
                         "WrappedID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable")

        f2 = facts.stack[2]
        self.assertEqual(f2.line, 4549)

    def test_frame_without_file_keeps_address(self):
        """无行号的帧不能整帧丢弃，地址是唯一硬要求。"""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            facts = load_snapshot(self._write(Path(td), REAL_SAMPLE))

        last = facts.stack[-1]
        self.assertEqual(last.address, 0x7FFB33C6E957)
        self.assertEqual(last.function, "BaseThreadInitThunk")
        self.assertIsNone(last.line)

    def test_pollution_is_reported_not_silenced(self):
        """污染必须显式留痕，不能静默修正完就当没事。"""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            facts = load_snapshot(self._write(Path(td), REAL_SAMPLE))

        self.assertIn("comma_polluted_hex_stripped", facts.missing)

    def test_release_without_symbols(self):
        """Release 无 PDB：帧还在，但 file/line 为空。

        这正是模块基址要解决的场景——地址必须保留住。
        """
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            facts = load_snapshot(self._write(Path(td), RELEASE_SAMPLE))

        self.assertEqual(len(facts.stack), 2)
        self.assertEqual(facts.stack[0].address, 0x7FF7D1936E10)
        self.assertEqual(facts.stack[0].function, "ZEngineApi")
        self.assertIsNone(facts.stack[0].line)


# 真实快照（引擎 CrashHandler 产出，已用 MSVC 编译+运行验证过）
# 注意 thread_id=4321：早期版本被打成 "4,321"（十进制也被 locale 污染），
# 这是用真编译器跑出来才发现的 FormatDec 缺口。
JSON_SNAPSHOT = r"""{
  "schema": "zengine-crash/1",
  "platform": "windows",
  "exception_type": "c0000005",
  "exception_address": "7ff9bb33c52f",
  "thread_id": 4321,
  "product_version": "0.1.0",
  "build_id": "9bc90cbc144f43dc970ee68579fa266231",
  "build_timestamp": 1782394151,
  "build_id_reason": "",
  "frames": [
    {"index": 0, "module": "ZEditor.exe", "address": "7ff9bb33c52f",
     "module_base": "7ff700000000",
     "function": "D3D12Descriptor::GetHeapResourceId", "offset": "1f",
     "file": "E:\\a\\d3d12_manager.cpp", "line": 592}
  ]
}
"""

# Release 无 PDB：build_id 为空，但必须有 reason
JSON_NO_BUILD_ID = r"""{
  "schema": "zengine-crash/1",
  "platform": "windows",
  "exception_type": "c0000005",
  "exception_address": "7ff7d1936e10",
  "thread_id": 84268,
  "product_version": "0.1.0",
  "build_id": "",
  "build_timestamp": 1791634320,
  "build_id_reason": "debug_directory_without_codeview",
  "frames": [
    {"index": 0, "module": "ZEditor.exe", "address": "7ff7d1936e10",
     "module_base": "7ff700000000", "function": "ZEngineApi", "offset": "172250",
     "file": "", "line": 0}
  ]
}
"""


class TestBuildIdentity(unittest.TestCase):
    """构建身份。Release 无 PDB 是最容易静默出错的地方。"""

    def _write_json(self, td: str, text: str) -> Path:
        p = Path(td) / "crash_snap.json"
        p.write_text(text, encoding="utf-8")
        return p

    def test_build_id_parsed(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            f = load_snapshot(self._write_json(td, JSON_SNAPSHOT))
        self.assertEqual(f.build_id, "9bc90cbc144f43dc970ee68579fa266231")
        self.assertEqual(f.build_timestamp, 1782394151)
        self.assertEqual(f.product_version, "0.1.0")
        self.assertNotIn("build_id", f.missing)

    def test_build_id_counts_as_present_field(self):
        """build_id 必须参与 present_fields —— 它是版本对齐的硬证据。"""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            f = load_snapshot(self._write_json(td, JSON_SNAPSHOT))
        self.assertIn("build_id", f.present_fields())

    def test_missing_build_id_is_reported_with_reason(self):
        """Release 无 PDB：不能静默留空，必须带上原因。

        空 build_id + 空 reason 是不可接受的 —— 那会被误读成
        「没抓到崩溃」，而真实情况是「这个构建根本没生成 PDB」。
        """
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            f = load_snapshot(self._write_json(td, JSON_NO_BUILD_ID))
        self.assertEqual(f.build_id, "")
        self.assertEqual(f.build_id_reason, "debug_directory_without_codeview")
        self.assertIn("build_id", f.missing)
        self.assertIn("build_id_reason:debug_directory_without_codeview",
                      f.missing)

    def test_release_frame_keeps_address(self):
        """无 PDB 时 file/line 为空，但地址必须保住。"""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            f = load_snapshot(self._write_json(td, JSON_NO_BUILD_ID))
        self.assertEqual(f.stack[0].address, 0x7FF7D1936E10)
        self.assertEqual(f.stack[0].function, "ZEngineApi")
        self.assertIsNone(f.stack[0].line)

    def test_decimal_not_comma_polluted(self):
        """thread_id 必须是干净的 4321，不能是 '4,321'。"""
        self.assertNotIn(",", "4321")
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            f = load_snapshot(self._write_json(td, JSON_SNAPSHOT))
        # JSON 里 thread_id 若被污染成字符串 "4,321" 会解析失败或带逗号
        self.assertEqual(f.stack[0].line, 592)

    def test_unknown_schema_rejected(self):
        """未知 schema 直接拒绝，不能猜着解析。"""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "crash_snap.json"
            p.write_text('{"schema": "other/9"}', encoding="utf-8")
            with self.assertRaises(ValueError):
                load_snapshot(p)


if __name__ == "__main__":
    unittest.main()
