"""L1 视觉理解 provider（vision_provider.py）门禁。

只测确定性部分（prompt 构造 / JSON 抽取 / dict->Detection / mock provider /
图片尺寸探测），不真调混元（需要 HUNYUAN_API_KEY，留作集成测试）。
运行：python verify/test_vision_provider.py
"""

from __future__ import annotations

import base64
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import decompose  # noqa: E402
import vision_provider as vp  # noqa: E402


class PromptTests(unittest.TestCase):
    def test_prompt_contains_category_hint(self) -> None:
        p = vp.build_detection_prompt(1920, 864)
        self.assertIn("ground", p)
        self.assertIn("prop", p)
        self.assertIn("1920", p)
        self.assertIn("864", p)

    def test_prompt_demands_json_only(self) -> None:
        p = vp.build_detection_prompt(1920, 864)
        self.assertIn("JSON 数组", p)
        self.assertIn("markdown", p)  # 禁止代码块


class JsonExtractTests(unittest.TestCase):
    def test_extract_plain_array(self) -> None:
        items = vp._extract_json_array('[{"bbox":[0,0,1,1],"category":"prop"}]')
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["category"], "prop")

    def test_extract_with_markdown_fence(self) -> None:
        text = '好的，结果如下：\n```json\n[{"bbox":[0,0,1,1],"category":"ground"}]\n```\n'
        items = vp._extract_json_array(text)
        self.assertEqual(items[0]["category"], "ground")

    def test_extract_with_surrounding_prose(self) -> None:
        text = '检测到 2 个物体 [{"bbox":[0,0,1,1],"category":"a"},{"bbox":[2,2,3,3],"category":"b"}] 完毕'
        items = vp._extract_json_array(text)
        self.assertEqual(len(items), 2)

    def test_extract_no_array_raises(self) -> None:
        with self.assertRaises(vp.VisionParseError):
            vp._extract_json_array("没有数组")

    def test_extract_not_a_list_raises(self) -> None:
        with self.assertRaises(vp.VisionParseError):
            vp._extract_json_array('{"a": 1}')


class DictToDetectionTests(unittest.TestCase):
    def test_valid_dict(self) -> None:
        d = vp._dict_to_detection(
            {"bbox": [0, 0, 10, 10], "category": "prop", "confidence": 0.8,
             "depth_m": 5.0, "label": "x"})
        self.assertEqual(d.category, "prop")
        self.assertEqual(d.confidence, 0.8)
        self.assertEqual(d.depth_m, 5.0)
        self.assertEqual(d.label, "x")

    def test_missing_confidence_defaults(self) -> None:
        d = vp._dict_to_detection({"bbox": [0, 0, 1, 1], "category": "prop"})
        self.assertEqual(d.confidence, 0.5)

    def test_bad_bbox_raises(self) -> None:
        with self.assertRaises(vp.VisionParseError):
            vp._dict_to_detection({"bbox": [0, 0], "category": "prop"})


class MockProviderTests(unittest.TestCase):
    def test_mock_provider_returns_detections(self) -> None:
        p = vp.MockVisionProvider()
        dets = p.detect("whatever.png")
        self.assertGreaterEqual(len(dets), 5)
        for d in dets:
            self.assertTrue(0.0 <= d.confidence <= 1.0)

    def test_mock_provider_feeds_decompose(self) -> None:
        # mock 检测 -> 确定性装配 -> 合法 scene-ir（全链路，不调模型）
        dets = vp.MockVisionProvider().detect("x.png")
        ir = decompose.decompose_to_ir(dets, "a" * 64)
        self.assertEqual(ir["schema"], "zengine.scene-ir/v1")
        self.assertGreaterEqual(len(ir["nodes"]), 5)


class ImageSizeProbeTests(unittest.TestCase):
    def test_png_size_probe(self) -> None:
        # 手造一个 4x3 的 PNG（最小合法头）
        import struct
        import zlib
        w, h = 4, 3
        sig = b"\x89PNG\r\n\x1a\n"
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        ihdr_chunk = struct.pack(">I", 13) + b"IHDR" + ihdr + struct.pack(">I", zlib.crc32(b"IHDR" + ihdr))
        raw = b"".join(b"\x00" + b"\x00\x00\x00" * w for _ in range(h))
        idat = struct.pack(">I", len(raw)) + b"IDAT" + raw + struct.pack(">I", zlib.crc32(b"IDAT" + raw))
        iend = struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND"))
        png = sig + ihdr_chunk + idat + iend
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
            f.write(png)
            path = f.name
        try:
            self.assertEqual(vp.HunyuanVisionProvider._probe_image_size(path), (w, h))
        finally:
            os.unlink(path)


class EnvKeyTests(unittest.TestCase):
    def test_missing_key_raises(self) -> None:
        # 临时清掉 HUNYUAN_API_KEY，验证缺 key 时构造报错
        old = os.environ.pop("HUNYUAN_API_KEY", None)
        try:
            with self.assertRaises(RuntimeError):
                vp.HunyuanVisionProvider()
        finally:
            if old is not None:
                os.environ["HUNYUAN_API_KEY"] = old


if __name__ == "__main__":
    unittest.main(verbosity=2)
