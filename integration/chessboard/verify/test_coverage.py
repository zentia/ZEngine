"""成分覆盖对拍（coverage.py）门禁。

只测集合级语义成分对拍（name 精确 / kind+position 模糊 / missing / extra /
core 覆盖率），不涉及像素 IoU 与真实渲染。
运行：python verify/test_coverage.py
"""

from __future__ import annotations

import copy
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import coverage  # noqa: E402


def make_baseline() -> dict:
    return {
        "name": "测试基准",
        "source_image_sha256": "a" * 64,
        "components": [
            {"id": "b1", "name": "棋盘台面", "kind": "ground", "position": "center", "core": True},
            {"id": "b2", "name": "大红枫树", "kind": "structure", "position": "left", "core": True},
            {"id": "b3", "name": "树干灯阵", "kind": "prop", "position": "left", "core": False},
            {"id": "b4", "name": "斗笠", "kind": "prop", "position": "right", "core": False},
        ],
    }


class CoverageTests(unittest.TestCase):
    def test_full_coverage_passes(self) -> None:
        scene = {"name": "s", "components": [
            {"id": "s1", "name": "棋盘台面", "kind": "ground", "position": "center"},
            {"id": "s2", "name": "大红枫树", "kind": "structure", "position": "left"},
            {"id": "s3", "name": "树干灯阵", "kind": "prop", "position": "left"},
            {"id": "s4", "name": "斗笠", "kind": "prop", "position": "right"},
        ]}
        r = coverage.coverage(make_baseline(), scene)
        self.assertTrue(r["pass"])
        self.assertEqual(r["coverage_rate"], 1.0)
        self.assertEqual(r["missing"], 0)

    def test_missing_detected(self) -> None:
        scene = {"name": "s", "components": [
            {"id": "s1", "name": "棋盘台面", "kind": "ground", "position": "center"},
        ]}
        r = coverage.coverage(make_baseline(), scene)
        self.assertEqual(r["missing"], 3)
        self.assertFalse(r["pass"])

    def test_critical_missing_detected(self) -> None:
        # 核心成分（大红枫树 core=True）漏了 -> critical_missing >= 1
        scene = {"name": "s", "components": [
            {"id": "s3", "name": "树干灯阵", "kind": "prop", "position": "left"},
            {"id": "s4", "name": "斗笠", "kind": "prop", "position": "right"},
        ]}
        r = coverage.coverage(make_baseline(), scene)
        self.assertGreaterEqual(r["critical_missing"], 2)
        # 漏了 core 的棋盘台面 + 大红枫树
        names = [m["name"] for m in r["missing_items"]]
        self.assertIn("棋盘台面", names)
        self.assertIn("大红枫树", names)

    def test_extra_detected(self) -> None:
        scene = {"name": "s", "components": [
            {"id": "s1", "name": "棋盘台面", "kind": "ground", "position": "center"},
            {"id": "s2", "name": "大红枫树", "kind": "structure", "position": "left"},
            {"id": "s3", "name": "树干灯阵", "kind": "prop", "position": "left"},
            {"id": "s4", "name": "斗笠", "kind": "prop", "position": "right"},
            {"id": "s9", "name": "多放的石狮子", "kind": "prop", "position": "center"},
        ]}
        r = coverage.coverage(make_baseline(), scene)
        self.assertEqual(r["extra"], 1)
        self.assertEqual(r["extra_items"][0]["name"], "多放的石狮子")

    def test_exact_match_priority_over_kind_position(self) -> None:
        # 两个同 kind+position 的成分，name 精确匹配应优先，不错位
        baseline = {
            "name": "b",
            "source_image_sha256": "a" * 64,
            "components": [
                {"id": "b1", "name": "木质推车", "kind": "prop", "position": "left", "core": False},
                {"id": "b2", "name": "树干灯阵", "kind": "prop", "position": "left", "core": False},
            ],
        }
        scene = {"name": "s", "components": [
            {"id": "s1", "name": "树干灯阵", "kind": "prop", "position": "left"},
        ]}
        r = coverage.coverage(baseline, scene)
        # 树干灯阵 应 exact 匹配，木质推车 missing；而不是模糊匹配错位
        covered_names = [c["name"] for c in r["covered_items"]]
        self.assertIn("树干灯阵", covered_names)
        missing_names = [m["name"] for m in r["missing_items"]]
        self.assertIn("木质推车", missing_names)
        # 匹配方式应为 exact
        matched = next(c for c in r["covered_items"] if c["name"] == "树干灯阵")
        self.assertEqual(matched["matched_by"], "exact")

    def test_kind_position_fuzzy_match(self) -> None:
        # 名字不同但 kind+position 相同 -> 模糊匹配命中
        baseline = {
            "name": "b",
            "source_image_sha256": "a" * 64,
            "components": [
                {"id": "b1", "name": "左侧灯笼", "kind": "prop", "position": "left", "core": False},
            ],
        }
        scene = {"name": "s", "components": [
            {"id": "s1", "name": "一个灯", "kind": "prop", "position": "left"},
        ]}
        r = coverage.coverage(baseline, scene)
        self.assertEqual(r["missing"], 0)
        self.assertEqual(r["covered_items"][0]["matched_by"], "kind_position")


if __name__ == "__main__":
    unittest.main(verbosity=2)
