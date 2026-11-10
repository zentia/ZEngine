"""L5 补丁应用层（apply_patch.py）门禁。

只测确定性应用逻辑（契约校验 / 字段白名单 / frozen 拒绝 / set/delete/add
语义 / 数值类型校验），不涉及真实 Unity 渲染与 L4 对拍。
运行：python verify/test_apply_patch.py
"""

from __future__ import annotations

import copy
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import apply_patch  # noqa: E402


def make_ir() -> dict:
    """含 frozen 节点 + 可改节点 + 多放节点 的 assigned scene-ir。"""
    return {
        "schema": "zengine.scene-ir/v1",
        "source": {"image_sha256": "a" * 64, "views": 1},
        "units": "meter",
        "coordinate_system": "glTF Y-up right-handed",
        "nodes": [
            {
                "id": "core_slab", "kind": "ground", "confidence": 0.95,
                "geometry_ref": "board.zbrd#core_slab", "frozen": True,
            },
            {
                "id": "prop_lantern_w", "kind": "prop", "confidence": 0.98,
                "asset_id": "star_pillar", "anchor": [1.0, 0.0, 2.0], "scale": 1.0,
            },
            {
                "id": "prop_extra", "kind": "prop", "confidence": 0.9,
                "asset_id": "halo_ring", "anchor": [0.0, 0.0, 0.0],
            },
        ],
    }


def make_patch(patches: list) -> dict:
    return {
        "schema": "zengine.patch-suggestion/v1",
        "source_report_sha256": "b" * 64,
        "patches": patches,
    }


class ValidatePatchTests(unittest.TestCase):
    def test_valid_set_patch(self) -> None:
        p = make_patch([
            {"op": "set", "node_id": "prop_lantern_w", "field": "anchor",
             "value": [1.5, 0.0, 2.5], "reason": "位置偏移"},
        ])
        self.assertEqual(apply_patch.validate_patch(p), [])

    def test_unknown_field_rejected(self) -> None:
        p = make_patch([
            {"op": "set", "node_id": "x", "field": "confidence",
             "value": 0.5, "reason": "试图改身份字段"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(any("白名单" in e for e in errs))

    def test_identity_field_rejected(self) -> None:
        # id 不在白名单，但即使进了也会被 identity 判定拒绝
        p = make_patch([
            {"op": "set", "node_id": "x", "field": "id",
             "value": "y", "reason": "改 id"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(len(errs) > 0)

    def test_anchor_wrong_arity_rejected(self) -> None:
        p = make_patch([
            {"op": "set", "node_id": "x", "field": "anchor",
             "value": [1.0, 2.0], "reason": "错"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(any("3 元素" in e for e in errs))

    def test_scale_negative_rejected(self) -> None:
        p = make_patch([
            {"op": "set", "node_id": "x", "field": "scale",
             "value": -1.0, "reason": "负尺度"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(any("scale" in e for e in errs))

    def test_bad_lod_rejected(self) -> None:
        p = make_patch([
            {"op": "set", "node_id": "x", "field": "lod",
             "value": "Ultra", "reason": "非法 lod"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(any("lod" in e for e in errs))

    def test_delete_with_field_rejected(self) -> None:
        p = make_patch([
            {"op": "delete", "node_id": "x", "field": "anchor",
             "reason": "delete 不应带 field"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(len(errs) > 0)

    def test_add_node_id_mismatch_rejected(self) -> None:
        p = make_patch([
            {"op": "add", "node_id": "a",
             "node": {"id": "b", "kind": "prop", "confidence": 0.9},
             "reason": "id 不一致"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(any("不一致" in e for e in errs))

    def test_unknown_op_rejected(self) -> None:
        p = make_patch([
            {"op": "move", "node_id": "x", "reason": "未知 op"},
        ])
        errs = apply_patch.validate_patch(p)
        self.assertTrue(any("未知 op" in e for e in errs))


class ApplyPatchTests(unittest.TestCase):
    def test_set_anchor_applied(self) -> None:
        ir = make_ir()
        p = make_patch([
            {"op": "set", "node_id": "prop_lantern_w", "field": "anchor",
             "value": [1.5, 0.0, 2.5], "reason": "偏移"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["applied_count"], 1)
        self.assertEqual(report["rejected_count"], 0)
        node = next(n for n in patched["nodes"] if n["id"] == "prop_lantern_w")
        self.assertEqual(node["anchor"], [1.5, 0.0, 2.5])

    def test_frozen_node_rejected(self) -> None:
        ir = make_ir()
        p = make_patch([
            {"op": "set", "node_id": "core_slab", "field": "scale",
             "value": 2.0, "reason": "改冻结几何"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["applied_count"], 0)
        self.assertEqual(report["rejected_count"], 1)
        self.assertIn("冻结", report["rejected"][0]["reason"])
        # 原节点未被改动
        node = next(n for n in patched["nodes"] if n["id"] == "core_slab")
        self.assertNotIn("scale", node)

    def test_delete_extra_node(self) -> None:
        ir = make_ir()
        p = make_patch([
            {"op": "delete", "node_id": "prop_extra", "reason": "多放"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["applied_count"], 1)
        ids = [n["id"] for n in patched["nodes"]]
        self.assertNotIn("prop_extra", ids)

    def test_add_missing_node(self) -> None:
        ir = make_ir()
        p = make_patch([
            {"op": "add", "node_id": "prop_lantern_c",
             "node": {"id": "prop_lantern_c", "kind": "prop", "confidence": 0.95,
                      "asset_id": "star_pillar", "anchor": [0.0, 0.0, 0.0]},
             "reason": "漏放"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["applied_count"], 1)
        ids = [n["id"] for n in patched["nodes"]]
        self.assertIn("prop_lantern_c", ids)

    def test_add_conflict_rejected(self) -> None:
        ir = make_ir()
        p = make_patch([
            {"op": "add", "node_id": "core_slab",
             "node": {"id": "core_slab", "kind": "ground", "confidence": 0.9},
             "reason": "重复 id"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["rejected_count"], 1)
        self.assertIn("已存在", report["rejected"][0]["reason"])

    def test_set_nonexistent_node_rejected(self) -> None:
        ir = make_ir()
        p = make_patch([
            {"op": "set", "node_id": "no_such", "field": "scale",
             "value": 2.0, "reason": "不存在"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["rejected_count"], 1)
        self.assertIn("不存在", report["rejected"][0]["reason"])

    def test_partial_failure_does_not_abort_others(self) -> None:
        # 一条失败（frozen），一条成功（合法 set），失败不中断其余
        ir = make_ir()
        p = make_patch([
            {"op": "set", "node_id": "core_slab", "field": "scale",
             "value": 2.0, "reason": "冻结，应失败"},
            {"op": "set", "node_id": "prop_lantern_w", "field": "scale",
             "value": 3.0, "reason": "合法"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["applied_count"], 1)
        self.assertEqual(report["rejected_count"], 1)
        node = next(n for n in patched["nodes"] if n["id"] == "prop_lantern_w")
        self.assertEqual(node["scale"], 3.0)

    def test_original_ir_not_mutated(self) -> None:
        ir = make_ir()
        snapshot = copy.deepcopy(ir)
        p = make_patch([
            {"op": "set", "node_id": "prop_lantern_w", "field": "scale",
             "value": 9.0, "reason": "改"},
        ])
        apply_patch.apply_patch(ir, p)
        self.assertEqual(ir, snapshot)  # 原始 ir 不被污染


class AssignmentFrozenTests(unittest.TestCase):
    def test_assignment_status_frozen_rejected(self) -> None:
        # L2 判定的 frozen（assignment.status == frozen）也要拒绝
        ir = make_ir()
        ir["nodes"].append({
            "id": "core_slab2", "kind": "ground", "confidence": 0.9,
            "assignment": {"status": "frozen", "reason": "core slab"},
        })
        p = make_patch([
            {"op": "set", "node_id": "core_slab2", "field": "scale",
             "value": 2.0, "reason": "改 frozen"},
        ])
        patched, report = apply_patch.apply_patch(ir, p)
        self.assertEqual(report["rejected_count"], 1)
        self.assertIn("冻结", report["rejected"][0]["reason"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
