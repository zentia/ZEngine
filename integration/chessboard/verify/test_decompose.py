"""L1 分解层（decompose.py）确定性装配的门禁。

纯 stdlib，不调真实模型。锁：类别 -> kind 映射、置信度阈值 + unknown 降级、
bbox 归一化、深度排序、反投影的诚实边界（缺相机不硬算 world）。
运行：python verify/test_decompose.py
"""

from __future__ import annotations

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import decompose as dc  # noqa: E402


class KindMappingTests(unittest.TestCase):
    def test_known_categories_map(self) -> None:
        self.assertEqual(dc.map_kind("board"), "ground")
        self.assertEqual(dc.map_kind("slab"), "ground")
        self.assertEqual(dc.map_kind("pillar"), "structure")
        self.assertEqual(dc.map_kind("lantern"), "prop")
        self.assertEqual(dc.map_kind("character"), "character")
        self.assertEqual(dc.map_kind("vfx"), "vfx_emitter")

    def test_unknown_category_maps_to_unknown(self) -> None:
        self.assertEqual(dc.map_kind("something_unknown"), "unknown")
        self.assertEqual(dc.map_kind(""), "unknown")

    def test_mapping_is_case_insensitive(self) -> None:
        self.assertEqual(dc.map_kind("Lantern"), "prop")


class DecomposeTests(unittest.TestCase):
    def test_low_confidence_downgraded_to_unknown(self) -> None:
        dets = [
            dc.Detection(bbox=[0, 0, 10, 10], category="board", confidence=0.42),
        ]
        ir = dc.decompose_to_ir(dets, "a" * 64, threshold=0.5)
        self.assertEqual(ir["nodes"][0]["kind"], "unknown")
        self.assertIn("reason", ir["nodes"][0])
        # unknown 不得带 provenance / anchor
        self.assertNotIn("provenance", ir["nodes"][0])
        self.assertNotIn("anchor", ir["nodes"][0])

    def test_high_confidence_gets_provenance(self) -> None:
        dets = [
            dc.Detection(bbox=[0, 0, 10, 10], category="board", confidence=0.9),
        ]
        ir = dc.decompose_to_ir(dets, "a" * 64)
        node = ir["nodes"][0]
        self.assertEqual(node["kind"], "ground")
        self.assertIn("provenance", node)
        self.assertEqual(node["provenance"]["origin"], "hunyuan_vision")

    def test_character_and_vfx_deferred_to_unknown(self) -> None:
        # character 需要 rig/animation_slots、vfx_emitter 需要 emitter/timeline，
        # 都是 L2 生成器才有的字段，L1 单视角不能判定 -> 降级 unknown。
        dets = [
            dc.Detection(bbox=[0, 0, 10, 10], category="character", confidence=0.95),
            dc.Detection(bbox=[0, 0, 10, 10], category="vfx", confidence=0.95),
        ]
        ir = dc.decompose_to_ir(dets, "a" * 64)
        for node in ir["nodes"]:
            self.assertEqual(node["kind"], "unknown")
            self.assertIn("reason", node)

    def test_depth_sorting_far_to_near(self) -> None:
        dets = [
            dc.Detection(bbox=[0, 0, 10, 10], category="lantern", confidence=0.8, depth_m=5.0, label="near"),
            dc.Detection(bbox=[0, 0, 10, 10], category="board", confidence=0.9, depth_m=20.0, label="far"),
        ]
        ir = dc.decompose_to_ir(dets, "a" * 64)
        # 远的（depth 大）在前
        self.assertEqual(ir["nodes"][0]["label"], "far")
        self.assertEqual(ir["nodes"][1]["label"], "near")

    def test_depth_rank_sorting_far_to_near(self) -> None:
        # depth_rank 是主路径：0=最远在前，N=最近在后
        dets = [
            dc.Detection(bbox=[0, 0, 10, 10], category="lantern", confidence=0.8, depth_rank=5, label="near"),
            dc.Detection(bbox=[0, 0, 10, 10], category="board", confidence=0.9, depth_rank=0, label="far"),
            dc.Detection(bbox=[0, 0, 10, 10], category="ground", confidence=0.9, depth_rank=2, label="mid"),
        ]
        ir = dc.decompose_to_ir(dets, "a" * 64)
        labels = [n["label"] for n in ir["nodes"]]
        self.assertEqual(labels, ["far", "mid", "near"])

    def test_depth_rank_overrides_depth_m(self) -> None:
        # 有 depth_rank 时优先用 rank（rank=0 的远，即使 depth_m 标成近）
        dets = [
            dc.Detection(bbox=[0, 0, 10, 10], category="board", confidence=0.9,
                         depth_rank=0, depth_m=5.0, label="rank_far"),
            dc.Detection(bbox=[0, 0, 10, 10], category="lantern", confidence=0.8,
                         depth_rank=5, depth_m=50.0, label="rank_near"),
        ]
        ir = dc.decompose_to_ir(dets, "a" * 64)
        self.assertEqual(ir["nodes"][0]["label"], "rank_far")
        self.assertEqual(ir["nodes"][1]["label"], "rank_near")

    def test_bbox_normalized_in_bounds(self) -> None:
        dets = [dc.Detection(bbox=[480, 200, 960, 400], category="prop", confidence=0.7)]
        ir = dc.decompose_to_ir(dets, "a" * 64, image_size=(1920, 864))
        node = ir["nodes"][0]
        self.assertEqual(node["bbox_px"], [480.0, 200.0, 960.0, 400.0])

    def test_no_camera_no_anchor(self) -> None:
        # 缺相机参数：只留 bbox，不写 anchor（诚实边界）
        dets = [dc.Detection(bbox=[0, 0, 10, 10], category="board", confidence=0.9)]
        ir = dc.decompose_to_ir(dets, "a" * 64)
        self.assertNotIn("anchor", ir["nodes"][0])

    def test_with_camera_projects_anchor(self) -> None:
        camera = {
            "fov": 31.0,
            "pitch_deg": 50.0,
            "position": [0.0, 22.46, -19.64],
            "target": [0.0, 0.0, -0.794],
        }
        dets = [dc.Detection(bbox=[960, 700, 980, 720], category="board", confidence=0.9)]
        ir = dc.decompose_to_ir(dets, "a" * 64, camera=camera, image_size=(1920, 864))
        node = ir["nodes"][0]
        self.assertIn("anchor", node)
        # anchor 必须落在地面 y=0
        self.assertEqual(node["anchor"][1], 0.0)


class BackProjectTests(unittest.TestCase):
    def setUp(self) -> None:
        self.camera = {
            "fov": 31.0,
            "pitch_deg": 50.0,
            "position": [0.0, 22.46, -19.64],
            "target": [0.0, 0.0, -0.794],
        }

    def test_missing_camera_returns_none(self) -> None:
        self.assertIsNone(dc.back_project_ground([0, 0, 10, 10], {}, (1920, 864)))
        self.assertIsNone(dc.back_project_ground([0, 0, 10, 10], {"fov": 31.0}, (1920, 864)))

    def test_center_bbox_projects_to_ground(self) -> None:
        proj = dc.back_project_ground([960, 700, 980, 720], self.camera, (1920, 864))
        self.assertIsNotNone(proj)
        self.assertEqual(proj["anchor"][1], 0.0)
        self.assertTrue(proj["estimate"])
        # 中心点反投影应在相机 target 附近（yaw 朝向 target）
        self.assertTrue(abs(proj["anchor"][0]) < 10.0)
        self.assertTrue(abs(proj["anchor"][2]) < 10.0)


class MockDataTests(unittest.TestCase):
    def test_mock_detections_are_valid(self) -> None:
        dets = dc.make_mock_detections()
        self.assertGreaterEqual(len(dets), 5)
        for d in dets:
            self.assertTrue(0.0 <= d.confidence <= 1.0)
            self.assertEqual(len(d.bbox), 4)

    def test_mock_decomposes_to_valid_ir(self) -> None:
        dets = dc.make_mock_detections()
        ir = dc.decompose_to_ir(dets, "a" * 64)
        # 低置信度的 character 和未知类别都应降级 unknown
        kinds = [n["kind"] for n in ir["nodes"]]
        self.assertIn("unknown", kinds)
        # ground 应在最前（深度最大）
        self.assertEqual(ir["nodes"][0]["kind"], "ground")


if __name__ == "__main__":
    unittest.main(verbosity=2)
