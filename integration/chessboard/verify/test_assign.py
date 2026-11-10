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
         "mesh_sha256": "a" * 64, "pivot_offset": [0.0, 0.0, 0.0],
         "bounds": {"min": [-0.28, 0.0, -0.28], "max": [0.28, 1.063, 0.28]}},
        {"asset_id": "halo_ring", "label": "悬浮星象圆环",
         "mesh_sha256": "b" * 64, "pivot_offset": [0.0, 0.0, 0.0],
         "bounds": {"min": [-0.57, 0.0, -0.23], "max": [0.57, 1.121, 0.23]}},
        {"asset_id": "moon_orrery", "label": "月相星象仪",
         "mesh_sha256": "c" * 64, "pivot_offset": [0.0, 0.0, 0.0],
         "bounds": {"min": [-0.50, 0.0, -0.23], "max": [0.50, 1.093, 0.23]}},
        {"asset_id": "star_lantern", "label": "可点击星灯",
         "mesh_sha256": "d" * 64, "pivot_offset": [0.0, 0.0, 0.0],
         "bounds": {"min": [-0.47, 0.0, -0.34], "max": [0.47, 1.007, 0.34]}},
    ]
}

_CAMERA = {
    "fov": 31.0,
    "pitch_deg": 50.0,
    "position": [0.0, 22.46, -19.64],
    "target": [0.0, 0.0, -0.794],
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


class VisualLayerTests(unittest.TestCase):
    def test_ground_sky_is_visual(self) -> None:
        self.assertTrue(assign.is_visual_layer("夜空", "ground"))
        self.assertTrue(assign.is_visual_layer("星空背景", "ground"))

    def test_ground_terrain_is_visual(self) -> None:
        self.assertTrue(assign.is_visual_layer("石板地面", "ground"))
        self.assertTrue(assign.is_visual_layer("远山", "ground"))
        self.assertTrue(assign.is_visual_layer("地面装饰线", "ground"))

    def test_ground_material_is_visual(self) -> None:
        self.assertTrue(assign.is_visual_layer("水面倒影", "ground"))

    def test_structure_not_visual(self) -> None:
        # structure 是单体实体，即使 label 带「平台」也不误判为环境层
        self.assertFalse(assign.is_visual_layer("魔法平台", "structure"))

    def test_prop_not_visual(self) -> None:
        self.assertFalse(assign.is_visual_layer("月亮", "prop"))

    def test_ground_entity_not_visual(self) -> None:
        # ground kind 但语义是可摆放实体（罕见），不误判
        self.assertFalse(assign.is_visual_layer("神秘石碑", "ground"))

    def test_prop_decal_is_visual(self) -> None:
        # 「装饰线」是强信号材质词，即使 L1 归到 prop（decoration）也判环境层
        self.assertTrue(assign.is_visual_layer("地面装饰线", "prop"))

    def test_prop_positional_modifier_not_visual(self) -> None:
        # prop 上的弱信号词「地面」是位置修饰，实体（雕像）仍是单体
        self.assertFalse(assign.is_visual_layer("地面雕像", "prop"))


class FrozenGeometryTests(unittest.TestCase):
    def test_structure_platform_frozen(self) -> None:
        self.assertTrue(assign.is_frozen_geometry("魔法平台", "structure"))
        self.assertTrue(assign.is_frozen_geometry("棋盘本体", "structure"))

    def test_ground_slab_frozen(self) -> None:
        self.assertTrue(assign.is_frozen_geometry("核心台面", "ground"))

    def test_prop_not_frozen(self) -> None:
        # prop 不可能是棋盘本体冻结几何
        self.assertFalse(assign.is_frozen_geometry("平台", "prop"))

    def test_unrelated_not_frozen(self) -> None:
        self.assertFalse(assign.is_frozen_geometry("灯笼", "structure"))
        self.assertFalse(assign.is_frozen_geometry("月亮", "structure"))


class SkyObjectTests(unittest.TestCase):
    def test_moon_is_sky(self) -> None:
        self.assertTrue(assign.is_sky_object("月亮"))
        self.assertTrue(assign.is_sky_object("满月"))

    def test_sun_star_is_sky(self) -> None:
        self.assertTrue(assign.is_sky_object("太阳"))
        self.assertTrue(assign.is_sky_object("星星"))

    def test_entity_not_sky(self) -> None:
        self.assertFalse(assign.is_sky_object("灯笼"))
        self.assertFalse(assign.is_sky_object("雕像"))
        self.assertFalse(assign.is_sky_object("魔法平台"))


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

    def test_ground_sky_visual_layer(self) -> None:
        # 环境视觉层语义（夜空）-> visual_layer，不是 needs_generation
        node = {"id": "g1", "kind": "ground", "label": "夜空",
                "confidence": 0.95}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertNotIn("asset_id", out)
        self.assertEqual(out["assignment"]["status"], "visual_layer")

    def test_ground_terrain_visual_layer(self) -> None:
        node = {"id": "g2", "kind": "ground", "label": "石板地面",
                "confidence": 0.95}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertEqual(out["assignment"]["status"], "visual_layer")

    def test_structure_platform_is_frozen(self) -> None:
        # 「魔法平台」是棋盘本体 core_slab 冻结几何，不是可替换装饰物
        node = {"id": "s1", "kind": "structure", "label": "魔法平台",
                "confidence": 0.95}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertEqual(out["assignment"]["status"], "frozen")

    def test_prop_moon_is_sky(self) -> None:
        # 「月亮」是天空中的天体（sky@* 天穹），不是可摆放单体实体
        node = {"id": "p4", "kind": "prop", "label": "月亮",
                "confidence": 0.95}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
        self.assertEqual(out["assignment"]["status"], "sky")

    def test_prop_statue_needs_generation(self) -> None:
        # 真正的单体实体（雕像）未命中 -> needs_generation（图生 3D）
        node = {"id": "p5", "kind": "prop", "label": "英雄雕像",
                "confidence": 0.9}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 1080))
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
        # n1 ground「石板地面」-> visual_layer；n2 -> assigned；
        # n3 prop「神秘石碑」-> needs_generation；n4 -> skipped
        self.assertEqual(statuses[0], "visual_layer")
        self.assertEqual(statuses[1], "assigned")
        self.assertEqual(statuses[2], "needs_generation")
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


class IrToPlacementsTests(unittest.TestCase):
    def _assigned_ir(self) -> dict:
        scene_ir = {
            "schema": "zengine.scene-ir/v1",
            "nodes": [
                {"id": "n1", "kind": "prop", "label": "左侧灯笼",
                 "confidence": 0.9, "bbox_px": [100, 200, 200, 400]},
                {"id": "n2", "kind": "prop", "label": "神秘石碑",
                 "confidence": 0.8},
                {"id": "n3", "kind": "unknown", "confidence": 0.3},
            ],
        }
        return assign.assign_assets(scene_ir, _MANIFEST)

    def test_only_assigned_nodes_converted(self) -> None:
        ir = self._assigned_ir()
        placements = assign.ir_to_placements(ir)
        # 只有 n1（灯笼 -> assigned）被转换；n2 needs_generation、n3 skipped 跳过
        self.assertEqual(len(placements), 1)
        self.assertEqual(placements[0]["asset_id"], "star_lantern")
        self.assertEqual(placements[0]["instance_id"], "n1")

    def test_placement_has_honest_defaults(self) -> None:
        ir = self._assigned_ir()
        placements = assign.ir_to_placements(ir)
        p = placements[0]
        # 无相机 -> anchor 用占位 + anchor_pending；scale 默认 1.0 + scale_pending
        self.assertEqual(p["anchor"], [0.0, 0.0, 0.0])
        self.assertTrue(p["anchor_pending"])
        self.assertEqual(p["scale"], 1.0)
        self.assertTrue(p["scale_pending"])
        self.assertEqual(p["rotation_y_deg"], 0.0)

    def test_placement_carries_anchor_when_present(self) -> None:
        # 手工构造一个已带 anchor 的节点，验证 placements 透传 anchor
        ir = {
            "schema": "zengine.scene-ir/v1",
            "nodes": [
                {"id": "p1", "kind": "prop", "label": "灯笼", "confidence": 0.9,
                 "asset_id": "star_lantern", "anchor": [1.0, 0.0, 2.0],
                 "assignment": {"status": "assigned", "asset_id": "star_lantern"}},
            ],
        }
        placements = assign.ir_to_placements(ir)
        self.assertEqual(placements[0]["anchor"], [1.0, 0.0, 2.0])
        self.assertFalse(placements[0]["anchor_pending"])


class ScaleEstimateTests(unittest.TestCase):
    def test_scale_estimated_with_camera_and_bounds(self) -> None:
        # 有相机 + bbox + 资产 bounds -> scale 被反推，落入合理区间
        node = {"id": "p1", "kind": "prop", "label": "灯笼",
                "confidence": 0.9, "bbox_px": [900, 600, 1020, 720]}
        out = assign.assign_node(node, _MANIFEST, _CAMERA, (1920, 864))
        self.assertIn("scale", out)
        self.assertGreater(out["scale"], 0.0)
        self.assertFalse(out["scale_pending"])

    def test_scale_pending_without_camera(self) -> None:
        # 无相机 -> scale 保持 1.0 + scale_pending
        node = {"id": "p1", "kind": "prop", "label": "灯笼",
                "confidence": 0.9, "bbox_px": [900, 600, 1020, 720]}
        out = assign.assign_node(node, _MANIFEST, None, (1920, 864))
        self.assertEqual(out["scale"], 1.0)
        self.assertTrue(out["scale_pending"])

    def test_scale_in_reasonable_range(self) -> None:
        # 反推的 scale 必须在 [0.01, 100] 合理区间内（数值保护）
        node = {"id": "p1", "kind": "prop", "label": "灯笼",
                "confidence": 0.9, "bbox_px": [900, 600, 1020, 720]}
        out = assign.assign_node(node, _MANIFEST, _CAMERA, (1920, 864))
        self.assertGreaterEqual(out["scale"], 0.01)
        self.assertLessEqual(out["scale"], 100.0)


class VisualFieldMapTests(unittest.TestCase):
    def test_moon_maps_to_skybox(self) -> None:
        field, _ = assign.visual_field_for("月亮")
        self.assertEqual(field, "skybox")

    def test_night_sky_maps_to_top_color(self) -> None:
        field, _ = assign.visual_field_for("夜空")
        self.assertEqual(field, "skybox.top_color")

    def test_stone_slab_maps_to_core_slab(self) -> None:
        field, _ = assign.visual_field_for("石板地面")
        self.assertEqual(field, "materials.core_slab")

    def test_decal_maps_to_materials(self) -> None:
        field, _ = assign.visual_field_for("地面装饰线")
        self.assertEqual(field, "materials")

    def test_unmapped_returns_none(self) -> None:
        self.assertIsNone(assign.visual_field_for("英雄雕像"))


class VisualOverridesTests(unittest.TestCase):
    def _assigned_ir(self) -> dict:
        scene_ir = {
            "schema": "zengine.scene-ir/v1",
            "nodes": [
                {"id": "g1", "kind": "ground", "label": "夜空", "confidence": 0.95},
                {"id": "p1", "kind": "prop", "label": "月亮", "confidence": 0.95},
                {"id": "g2", "kind": "ground", "label": "石板地面", "confidence": 0.95},
                {"id": "g3", "kind": "ground", "label": "远山", "confidence": 0.90},
                {"id": "p2", "kind": "prop", "label": "地面装饰线", "confidence": 0.85},
                {"id": "l1", "kind": "prop", "label": "灯笼", "confidence": 0.9},
            ],
        }
        return assign.assign_assets(scene_ir, _MANIFEST)

    def test_only_visual_nodes_converted(self) -> None:
        overrides = assign.ir_to_visual_overrides(self._assigned_ir())
        # 夜空/月亮/石板地面/远山/地面装饰线 5 个 visual_layer/sky；
        # 灯笼是 assigned，不参与
        self.assertEqual(overrides["source_node_count"], 5)
        labels = [o["label"] for o in overrides["overrides"]]
        self.assertIn("夜空", labels)
        self.assertIn("月亮", labels)
        self.assertIn("石板地面", labels)
        self.assertIn("远山", labels)
        self.assertIn("地面装饰线", labels)
        self.assertNotIn("灯笼", labels)

    def test_override_has_target_field_and_null_color(self) -> None:
        overrides = assign.ir_to_visual_overrides(self._assigned_ir())
        by_label = {o["label"]: o for o in overrides["overrides"]}
        # 颜色留空（不反推，单目抽色不可信）
        self.assertIsNone(by_label["石板地面"]["color"])
        self.assertEqual(by_label["石板地面"]["target_field"], "materials.core_slab")
        self.assertEqual(by_label["月亮"]["target_field"], "skybox")
        self.assertEqual(by_label["夜空"]["target_field"], "skybox.top_color")
        self.assertEqual(by_label["地面装饰线"]["target_field"], "materials")

    def test_override_carries_assignment_status(self) -> None:
        overrides = assign.ir_to_visual_overrides(self._assigned_ir())
        by_label = {o["label"]: o for o in overrides["overrides"]}
        self.assertEqual(by_label["月亮"]["assignment"], "sky")
        self.assertEqual(by_label["夜空"]["assignment"], "visual_layer")

    def test_empty_ir_gives_empty_overrides(self) -> None:
        overrides = assign.ir_to_visual_overrides({"nodes": []})
        self.assertEqual(overrides["source_node_count"], 0)
        self.assertEqual(overrides["overrides"], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
