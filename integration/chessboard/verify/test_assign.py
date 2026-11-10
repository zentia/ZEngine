"""L2 资产生成编排层（assign.py）门禁。

只测确定性部分（label->asset_id 匹配 / 节点指派 / 拼现有资产 / 未命中降级），
不接图生 3D 模型（留作后续集成测试）。
运行：python verify/test_assign.py
"""

from __future__ import annotations

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import assign  # noqa: E402


_MANIFEST = {
    "assets": [
        {"asset_id": "star_pillar", "label": "星象灯柱",
         "mesh_sha256": "a" * 64, "pivot_offset": [0.0, 0.0, 0.0]},
        {"asset_id": "halo_ring", "label": "悬浮星象圆环",
         "mesh_sha256": "b" * 64, "pivot_offset": [0.0, 0.0, 0.0]},
        {"asset_id": "moon_orrery", "label": "月相星象仪",
         "mesh_sha256": "c" * 64, "pivot_offset": [0.0, 0.0, 0.0]},
        {"asset_id": "star_lantern", "label": "可点击星灯",
         "mesh_sha256": "d" * 64, "pivot_offset": [0.0, 0.0, 0.0]},
    ]
}


class LabelMatchTests(unittest.TestCase):
    def test_match_lantern(self) -> None:
        self.assertEqual(assign.match_asset("左侧灯笼", ["star_lantern"]),
                         "star_lantern")

    def test_match_pillar(self) -> None:
        self.assertEqual(assign.match_asset("星象灯柱", ["star_pillar"]),
                         "star_pillar")

    def test_match_halo(self) -> None:
        self.assertEqual(assign.match_asset("地面光环", ["halo_ring"]),
                         "halo_ring")

    def test_match_specific_before_generic(self) -> None:
        # 「灯」是泛化兜底，「灯柱」更具体，应优先命中 star_pillar
        ids = ["star_pillar", "star_lantern"]
        self.assertEqual(assign.match_asset("灯柱", ids), "star_pillar")
        self.assertEqual(assign.match_asset("灯笼", ids), "star_lantern")

    def test_no_match_returns_none(self) -> None:
        self.assertIsNone(assign.match_asset("不存在的物体", ["star_pillar"]))

    def test_match_requires_available(self) -> None:
        # 关键词命中但 asset 不在可用列表 -> 不指派
        self.assertIsNone(assign.match_asset("灯笼", ["star_pillar"]))


class AssignNodeTests(unittest.TestCase):
    def test_assignable_kind_gets_asset(self) -> None:
        node = {"id": "p1", "kind": "prop", "label": "左侧灯笼",
                "confidence": 0.9, "bbox_px": [0, 0, 10, 10]}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertEqual(out["asset_id"], "star_lantern")
        self.assertEqual(out["mesh_sha256"], "d" * 64)
        self.assertEqual(out["assignment"]["status"], "assigned")

    def test_unknown_kind_skipped(self) -> None:
        node = {"id": "u1", "kind": "unknown", "confidence": 0.3}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertNotIn("asset_id", out)
        self.assertEqual(out["assignment"]["status"], "skipped")

    def test_character_kind_skipped(self) -> None:
        # character 的 rig/animation_slots 是上层生成器职责，L2 不碰
        node = {"id": "c1", "kind": "character", "confidence": 0.9}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertEqual(out["assignment"]["status"], "skipped")

    def test_no_match_needs_generation(self) -> None:
        node = {"id": "p2", "kind": "prop", "label": "神秘石碑",
                "confidence": 0.8}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertNotIn("asset_id", out)
        self.assertEqual(out["assignment"]["status"], "needs_generation")

    def test_no_camera_no_anchor(self) -> None:
        # 缺相机：指派成功但不反投影 anchor（诚实边界）
        node = {"id": "p3", "kind": "prop", "label": "灯笼",
                "confidence": 0.9, "bbox_px": [0, 0, 10, 10]}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertNotIn("anchor", out)


class AssignAssetsTests(unittest.TestCase):
    def test_end_to_end_assign(self) -> None:
        scene_ir = {
            "schema": "zengine.scene-ir/v1",
            "nodes": [
                {"id": "n1", "kind": "ground", "label": "石板地面",
                 "confidence": 0.9},
                {"id": "n2", "kind": "prop", "label": "左侧灯笼",
                 "confidence": 0.9},
                {"id": "n3", "kind": "prop", "label": "神秘石碑",
                 "confidence": 0.8},
                {"id": "n4", "kind": "unknown", "confidence": 0.3},
            ],
        }
        out = assign.assign_assets(scene_ir, _MANIFEST)
        statuses = [n.get("assignment", {}).get("status") for n in out["nodes"]]
        # n1 ground 无匹配 -> needs_generation；n2 -> assigned；n3 -> needs_generation；n4 -> skipped
        self.assertEqual(statuses[1], "assigned")
        self.assertEqual(statuses[3], "skipped")
        self.assertEqual(out["nodes"][1]["asset_id"], "star_lantern")

    def test_original_nodes_preserved(self) -> None:
        scene_ir = {"schema": "zengine.scene-ir/v1", "nodes": [
            {"id": "n1", "kind": "prop", "label": "灯笼", "confidence": 0.9}]}
        out = assign.assign_assets(scene_ir, _MANIFEST)
        self.assertEqual(out["schema"], "zengine.scene-ir/v1")
        # 原节点字段（id/kind/label/confidence）都保留
        n = out["nodes"][0]
        self.assertEqual(n["id"], "n1")
        self.assertEqual(n["kind"], "prop")
        self.assertEqual(n["label"], "灯笼")
        self.assertEqual(n["confidence"], 0.9)


if __name__ == "__main__":
    unittest.main(verbosity=2)
