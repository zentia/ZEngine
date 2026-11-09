"""Scene IR P0 契约门禁：L0 真源 sha 锁 / schema / 业务规则 / 镜像漂移。

纯 stdlib，不依赖 Unity / three.js / jsonschema。运行：
  python verify/test_scene_ir.py
"""

from __future__ import annotations

import copy
import json
import os
import sys
import unittest
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import verify_scene_ir as v  # noqa: E402


class SceneIRContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = v.load_json(v.CONTRACT_PATH)
        cls.schema = v.load_json(v.SCHEMA_PATH)
        cls.example = v.load_json(v.EXAMPLE_PATH)
        cls.filled = v.load_json(v.HERE.parent / "scene-ir" / "scene-ir.example.filled.json")
        cls.asset_ids, _ = v.external_asset_ids()

    def test_contract_structure(self) -> None:
        self.assertEqual(v.validate_contract(self.contract), [])

    def test_schema_declares_draft07_shape(self) -> None:
        self.assertEqual(self.schema["$schema"], "http://json-schema.org/draft-07/schema#")
        for key in ("type", "required", "properties", "additionalProperties"):
            self.assertIn(key, self.schema)
        node = self.schema["properties"]["nodes"]["items"]
        self.assertEqual(node["properties"]["kind"]["enum"], self.contract["ir"]["kinds"])
        self.assertFalse(node["additionalProperties"])

    def test_example_passes_full_check(self) -> None:
        report = v.run_check(v.EXAMPLE_PATH, strict=False)
        self.assertEqual(report["errors"], [], json.dumps(report["errors"], ensure_ascii=False))
        self.assertTrue(report["ok"])
        self.assertGreater(report["warning_count"], 0, "单视角 + 相机未知应产生告警")

    def test_canonical_sources_are_sha_locked(self) -> None:
        errors, _, report = v.check_sources(self.contract)
        self.assertEqual(errors, [])
        canonical = [item for item in report if item["group"] == "canonical_sources"]
        self.assertEqual(len(canonical), 4)
        self.assertTrue(all(item["match"] for item in canonical))

    def test_mirror_drift_is_warning_by_default(self) -> None:
        canonical = {s["id"]: s["sha256"] for s in self.contract["canonical_sources"]}
        errors, warnings, _ = v.check_mirrors(self.contract, canonical, strict=False)
        self.assertEqual(errors, [])
        self.assertTrue(any("style-pack" in item for item in warnings))
        self.assertTrue(any("board-spec" in item for item in warnings))

    def test_mirror_drift_fails_in_strict_mode(self) -> None:
        canonical = {s["id"]: s["sha256"] for s in self.contract["canonical_sources"]}
        errors, _, _ = v.check_mirrors(self.contract, canonical, strict=True)
        self.assertGreater(len(errors), 0)
        report = v.run_check(v.EXAMPLE_PATH, strict=True)
        self.assertFalse(report["ok"])

    def test_placement_formula_matches_external_manifest(self) -> None:
        manifest = v.load_json(v.EXTERNAL_MANIFEST_PATH)
        self.assertIn("world = anchor + rotY", manifest["coordinate_system"])
        self.assertIn("pivot_offset", manifest["coordinate_system"])
        self.assertEqual(self.contract["ir"]["placement_formula"].split(" * ")[-1],
                         "(local - pivot_offset)")

    def test_ai_boundary_forbids_unity_outputs(self) -> None:
        forbidden = " ".join(self.contract["ai_boundary"]["forbidden_outputs"])
        for token in ("Unity", "board-spec.json", "ZBRD"):
            self.assertIn(token, forbidden)
        self.assertEqual(self.contract["ai_boundary"]["writable_files"], ["scene-ir.json"])

    def test_self_test_rules_all_intercepted(self) -> None:
        result = v.self_test()
        self.assertTrue(result["ok"], json.dumps(result, ensure_ascii=False))
        self.assertEqual(result["cases"], 15)
        self.assertEqual(result["passed"], 15)

    def test_rule_mutations(self) -> None:
        cases: list[tuple[str, Any, str]] = [
            ("duplicate id", lambda d: d["nodes"][1].update(id="ground_slab"), "id 重复"),
            ("low confidence", lambda d: d["nodes"][1].update(confidence=0.2), "必须降级为 kind=unknown"),
            ("unknown without reason", lambda d: d["nodes"][-1].pop("reason"), "unknown 节点必须写 reason"),
            ("frozen asset", lambda d: d["nodes"][0].update(asset_id="star_pillar"), "frozen 节点禁止带 asset_id"),
            ("unknown asset id", lambda d: d["nodes"][1].update(asset_id="not_exist"), "不在 external_mesh.manifest 白名单"),
            ("character without rig", lambda d: d["nodes"][2].pop("rig"), "character 节点必须带 rig"),
            ("vfx without timeline", lambda d: d["nodes"][3].pop("timeline"), "vfx_emitter 节点必须带 timeline"),
        ]
        for name, mutate, needle in cases:
            with self.subTest(case=name):
                doc = copy.deepcopy(self.example)
                mutate(doc)
                errors, _ = v.validate_ir(doc, self.contract, self.schema, self.asset_ids)
                self.assertTrue(any(needle in item for item in errors),
                                f"{name}: {errors}")

    def test_example_nodes_cover_three_asset_kinds(self) -> None:
        kinds = {node["kind"] for node in self.example["nodes"]}
        self.assertTrue({"ground", "prop", "character", "vfx_emitter", "unknown"} <= kinds)
        self.assertIn("unknown_03", {node["id"] for node in self.example["nodes"]})

    def test_empty_capture_profiles_is_warning_not_error(self) -> None:
        report = v.run_check(v.EXAMPLE_PATH, strict=False)
        self.assertEqual(report["errors"], [])
        self.assertTrue(any("capture_profiles" in item for item in report["warnings"]))
        self.assertIn("未配置 capture_profiles", " ".join(report["warnings"]))

    def test_filled_capture_profiles_pass(self) -> None:
        errors, warnings = v.validate_ir(self.filled, self.contract, self.schema, self.asset_ids)
        self.assertEqual(errors, [], json.dumps(errors, ensure_ascii=False))
        self.assertFalse(any("capture_profiles" in item for item in warnings),
                         "补全机位后不应再报 capture_profiles 告警")

    def test_capture_profile_role_enum(self) -> None:
        import copy as _copy
        doc = _copy.deepcopy(self.filled)
        doc["source"]["capture_profiles"][0]["role"] = "not_a_role"
        errors, _ = v.validate_ir(doc, self.contract, self.schema, self.asset_ids)
        self.assertTrue(any("enum" in item for item in errors),
                        f"非法 role 应被拦截：{errors}")

    def test_camera_is_optional_in_source(self) -> None:
        import copy as _copy
        doc = _copy.deepcopy(self.example)
        doc["source"].pop("camera", None)
        errors, _ = v.validate_ir(doc, self.contract, self.schema, self.asset_ids)
        self.assertEqual(errors, [], f"source 不带 camera 应合法：{errors}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
