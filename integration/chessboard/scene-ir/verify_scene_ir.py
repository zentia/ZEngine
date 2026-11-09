"""Scene IR P0 契约校验器（纯 stdlib，不依赖 jsonschema / Unity / three.js）。

职责：
  1. 校验 L0 契约自身结构、四份 canonical 真源的 sha256 锁；
  2. 校验 Scene IR 实例符合 draft-07 schema 的受支持子集；
  3. 校验业务规则（confidence 阈值 / unknown 降级 / provenance / frozen /
     锚点与 sha / 三类资产必填字段 / 相机参数缺失告警）；
  4. 报告 OSGame 已部署镜像的版本漂移（默认告警，--strict 才失败）。

用法：
  python scene-ir/verify_scene_ir.py --check
  python scene-ir/verify_scene_ir.py --ir path/to/scene-ir.json --strict
  python scene-ir/verify_scene_ir.py --refresh-sources   # 显式刷新 sha 锁
  python scene-ir/verify_scene_ir.py --self-test
  python verify/test_scene_ir.py                       # unittest 门禁

设计约束：**本文件不写任何 Unity 工程文件**。镜像只报告、不同步；
对齐已部署 Unity 资产需要重派生 ZBRD / Meshes / .unity，属于独立 step。
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CONTRACT_PATH = HERE / "contract.json"
SCHEMA_PATH = HERE / "scene-ir.schema.json"
EXAMPLE_PATH = HERE / "scene-ir.example.json"
EXTERNAL_MANIFEST_PATH = ROOT / "build" / "external_mesh.manifest.json"

HEX64 = re.compile(r"^[0-9a-f]{64}$")
ASSET_PLACEMENT_FIELDS = (
    "mesh_sha256", "pivot_offset", "anchor", "rotation_y_deg", "scale",
)
UNKNOWN_FORBIDDEN_FIELDS = (
    "asset_id", "mesh_sha256", "pivot_offset", "anchor", "rotation_y_deg",
    "scale", "geometry_ref", "rig", "animation_slots", "emitter", "timeline",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _type_ok(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    return False


def validate_shape(value: Any, schema: dict, path: str = "$") -> list[str]:
    """draft-07 受支持子集：type / enum / const / pattern / minimum / maximum /
    minItems / maxItems / items / required / properties / additionalProperties。

    刻意不引入第三方 jsonschema：与 verify/test_board.py 的纯 stdlib 门禁一致。
    """
    errors: list[str] = []
    expected_types = schema.get("type")
    if expected_types is not None:
        types = [expected_types] if isinstance(expected_types, str) else list(expected_types)
        if not any(_type_ok(value, item) for item in types):
            errors.append(f"{path}: type 不匹配，期望 {'|'.join(types)}，实际 {type(value).__name__}")
            return errors

    if "const" in schema and value != schema["const"]:
        errors.append(f"{path}: const 不匹配，期望 {schema['const']!r}")
    if "enum" in schema and not any(value == item for item in schema["enum"]):
        errors.append(f"{path}: 不在 enum {schema['enum']!r} 内")

    if isinstance(value, str) and "pattern" in schema:
        if not re.search(schema["pattern"], value):
            errors.append(f"{path}: 不匹配 pattern {schema['pattern']!r}")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: 小于 minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: 大于 maximum {schema['maximum']}")
    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append(f"{path}: 少于 minItems {schema['minItems']}")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append(f"{path}: 多于 maxItems {schema['maxItems']}")
        if "items" in schema:
            for index, item in enumerate(value):
                errors.extend(validate_shape(item, schema["items"], f"{path}[{index}]"))
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: 缺必填字段 {key}")
        properties = schema.get("properties", {})
        for key, child in value.items():
            if key in properties:
                errors.extend(validate_shape(child, properties[key], f"{path}.{key}"))
            elif schema.get("additionalProperties") is False:
                errors.append(f"{path}: 不允许的字段 {key}")
    return errors


def validate_contract(contract: dict) -> list[str]:
    errors: list[str] = []
    if contract.get("schema") != "zengine.scene-ir.contract/v1":
        errors.append("contract.schema 不是 zengine.scene-ir.contract/v1")
    required = ("canonical_sources", "ir", "ai_boundary", "free_dimensions",
                "frozen_dimensions", "asset_kinds", "rules", "mirrors")
    for key in required:
        if key not in contract:
            errors.append(f"contract 缺字段 {key}")
    sources = contract.get("canonical_sources", [])
    if len(sources) < 4:
        errors.append("canonical_sources 少于 4 份真源")
    for index, source in enumerate(sources):
        for key in ("id", "path", "sha256", "role"):
            if key not in source:
                errors.append(f"canonical_sources[{index}] 缺 {key}")
        if not HEX64.fullmatch(str(source.get("sha256", ""))):
            errors.append(f"canonical_sources[{index}].sha256 不是 64 位小写 hex")
    ir = contract.get("ir", {})
    if ir.get("document_schema") != "zengine.scene-ir/v1":
        errors.append("ir.document_schema 不是 zengine.scene-ir/v1")
    if not (0.0 < float(ir.get("confidence_unknown_threshold", 0)) < 1.0):
        errors.append("ir.confidence_unknown_threshold 必须落在 (0,1)")
    return errors


def check_sources(contract: dict) -> tuple[list[str], list[str], list[dict]]:
    errors: list[str] = []
    warnings: list[str] = []
    report: list[dict] = []
    for group, strict in (("canonical_sources", True), ("derived_sources", False)):
        for source in contract.get(group, []):
            path = ROOT / source["path"]
            item: dict[str, Any] = {"group": group, "id": source["id"], "path": source["path"]}
            if not path.is_file():
                item["exists"] = False
                errors.append(f"{group}:{source['id']} 文件不存在 {path}")
                report.append(item)
                continue
            actual = sha256_file(path)
            item.update({"exists": True, "expected": source["sha256"],
                         "actual": actual, "match": actual == source["sha256"]})
            if actual != source["sha256"]:
                message = f"{group}:{source['id']} sha 漂移（契约 {source['sha256'][:12]} / 磁盘 {actual[:12]}）"
                if strict:
                    errors.append(message + "；如属预期变更请显式 --refresh-sources")
                else:
                    warnings.append(message + "（派生产物，重跑生成器后需刷新 sha 锁）")
            report.append(item)
    return errors, warnings, report


def check_mirrors(contract: dict, canonical: dict[str, str],
                  strict: bool) -> tuple[list[str], list[str], list[dict]]:
    errors: list[str] = []
    warnings: list[str] = []
    report: list[dict] = []
    for mirror in contract.get("mirrors", []):
        state = mirror.get("state", "unknown")
        for entry in mirror.get("files", []):
            path = Path(entry["path"])
            item: dict[str, Any] = {"mirror": mirror["id"], "canonical_id": entry["canonical_id"],
                                    "path": entry["path"], "state": state}
            if not path.is_file():
                item["exists"] = False
                warnings.append(f"镜像 {mirror['id']} 文件不可达，跳过：{path}")
                report.append(item)
                continue
            actual = sha256_file(path)
            canonical_sha = canonical.get(entry["canonical_id"])
            item.update({"exists": True, "actual": actual,
                         "deployed": entry["deployed_sha256"], "canonical": canonical_sha})
            if actual != entry["deployed_sha256"]:
                errors.append(f"镜像 {mirror['id']}:{entry['canonical_id']} 已被改动，需重新取证并更新 contract.json")
            if canonical_sha is None:
                errors.append(f"镜像 {mirror['id']} 引用了不存在的 canonical_id {entry['canonical_id']}")
            elif actual != canonical_sha:
                message = (f"镜像 {mirror['id']}:{entry['canonical_id']} 与 canonical 不一致"
                           f"（state={state}，p0_action={mirror.get('p0_action')}）")
                if strict or state != "known_older_revision":
                    errors.append(message)
                else:
                    warnings.append(message + "；对齐需重派生 Unity 资产，P0 不覆盖")
            report.append(item)
    return errors, warnings, report


def external_asset_ids() -> tuple[set[str], list[str]]:
    if not EXTERNAL_MANIFEST_PATH.is_file():
        return set(), ["external_mesh.manifest.json 不可达，跳过 asset_id 白名单校验"]
    doc = load_json(EXTERNAL_MANIFEST_PATH)
    return {item["asset_id"] for item in doc.get("assets", [])}, []


def validate_ir(ir: Any, contract: dict, schema: dict,
                asset_ids: set[str] | None = None) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    errors.extend(validate_shape(ir, schema))
    if not isinstance(ir, dict):
        return errors, warnings

    ir_meta = contract["ir"]
    if ir.get("schema") != ir_meta["document_schema"]:
        errors.append(f"IR schema 与契约不一致：{ir.get('schema')!r}")
    if ir.get("units") != ir_meta["units"]:
        errors.append(f"IR units 与契约不一致：{ir.get('units')!r}")
    if ir.get("coordinate_system") != ir_meta["coordinate_system"]:
        errors.append("IR coordinate_system 与契约不一致")

    source = ir.get("source", {})
    camera = source.get("camera", {}) if isinstance(source, dict) else {}
    if isinstance(camera, dict):
        missing = [k for k in ("fov", "pitch_deg") if camera.get(k) is None]
        if missing:
            warnings.append(f"相机参数缺失 {missing}；L1 反投影前必须补齐，当前不可信")
    if isinstance(source, dict) and source.get("views", 1) < 2:
        warnings.append("仅 1 个视角；体量 / 遮挡 / 背面推断欠定，低置信度必须标 unknown")

    threshold = float(ir_meta["confidence_unknown_threshold"])
    kinds = set(ir_meta["kinds"])
    seen: set[str] = set()
    nodes = ir.get("nodes", [])
    if not isinstance(nodes, list):
        return errors, warnings
    for index, node in enumerate(nodes):
        where = f"nodes[{index}]"
        if not isinstance(node, dict):
            continue
        node_id = node.get("id", f"#{index}")
        if node_id in seen:
            errors.append(f"{where}: id 重复 {node_id}")
        seen.add(node_id)
        kind = node.get("kind")
        if kind not in kinds:
            errors.append(f"{where}: 非法 kind {kind!r}")
            continue
        confidence = node.get("confidence")
        if not isinstance(confidence, (int, float)) or isinstance(confidence, bool):
            errors.append(f"{where}: confidence 不是数字")
            continue
        if confidence < threshold and kind != "unknown":
            errors.append(f"{where}({node_id}): confidence {confidence} < {threshold}，必须降级为 kind=unknown")

        if kind == "unknown":
            if not node.get("reason"):
                errors.append(f"{where}({node_id}): unknown 节点必须写 reason")
            forbidden = [key for key in UNKNOWN_FORBIDDEN_FIELDS if key in node]
            if forbidden:
                errors.append(f"{where}({node_id}): unknown 节点禁止确定性字段 {forbidden}")
            continue

        provenance = node.get("provenance")
        if not isinstance(provenance, dict):
            errors.append(f"{where}({node_id}): 非 unknown 节点必须带 provenance")
        else:
            for key in ir_meta["required_provenance_fields"]:
                if not provenance.get(key):
                    errors.append(f"{where}({node_id}): provenance 缺 {key}")

        if node.get("frozen") and "asset_id" in node:
            errors.append(f"{where}({node_id}): frozen 节点禁止带 asset_id（glb 不得替代冻结几何）")

        if "asset_id" in node:
            for key in ASSET_PLACEMENT_FIELDS:
                if key not in node:
                    errors.append(f"{where}({node_id}): 带 asset_id 必须带 {key}")
            for key in ("anchor", "pivot_offset"):
                values = node.get(key, [])
                if not isinstance(values, list) or len(values) != 3:
                    errors.append(f"{where}({node_id}): {key} 必须是 3 个数字")
                elif not all(isinstance(v, (int, float)) and math.isfinite(float(v)) for v in values):
                    errors.append(f"{where}({node_id}): {key} 含非有限数值")
            for key in ("rotation_y_deg", "scale"):
                value = node.get(key)
                if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
                    errors.append(f"{where}({node_id}): {key} 不是有限数")
            if "scale" in node and isinstance(node["scale"], (int, float)) and node["scale"] <= 0:
                errors.append(f"{where}({node_id}): scale 必须 > 0")
            if asset_ids is not None and node["asset_id"] not in asset_ids:
                errors.append(f"{where}({node_id}): asset_id {node['asset_id']!r} 不在 external_mesh.manifest 白名单")

        for kind_name, required in (("character", ("rig", "animation_slots")),
                                    ("vfx_emitter", ("emitter", "timeline"))):
            if kind == kind_name:
                for key in required:
                    if key not in node:
                        errors.append(f"{where}({node_id}): {kind} 节点必须带 {key}")
    return errors, warnings


def run_check(ir_path: Path, strict: bool) -> dict:
    contract = load_json(CONTRACT_PATH)
    schema = load_json(SCHEMA_PATH)
    ir = load_json(ir_path)
    errors: list[str] = []
    warnings: list[str] = []

    errors.extend(validate_contract(contract))
    schema_meta_errors = []
    if schema.get("$schema") != "http://json-schema.org/draft-07/schema#":
        schema_meta_errors.append("schema 未声明 draft-07")
    for key in ("type", "required", "properties"):
        if key not in schema:
            schema_meta_errors.append(f"schema 缺 {key}")
    errors.extend([f"schema: {item}" for item in schema_meta_errors])

    src_errors, src_warnings, src_report = check_sources(contract)
    errors.extend(src_errors)
    warnings.extend(src_warnings)
    canonical = {item["id"]: item["sha256"] for item in contract["canonical_sources"]}
    mir_errors, mir_warnings, mir_report = check_mirrors(contract, canonical, strict)
    errors.extend(mir_errors)
    warnings.extend(mir_warnings)

    asset_ids, asset_warnings = external_asset_ids()
    warnings.extend(asset_warnings)
    ir_errors, ir_warnings = validate_ir(ir, contract, schema, asset_ids)
    errors.extend(ir_errors)
    warnings.extend(ir_warnings)

    return {
        "ok": not errors,
        "contract": str(CONTRACT_PATH),
        "ir_file": str(ir_path),
        "strict": strict,
        "sources": src_report,
        "mirrors": mir_report,
        "error_count": len(errors),
        "warning_count": len(warnings),
        "errors": errors,
        "warnings": warnings,
    }


def refresh_sources() -> dict:
    """显式重算 canonical / derived 的 sha 锁；镜像记录永不自动改写。"""
    contract = load_json(CONTRACT_PATH)
    changed = []
    for group in ("canonical_sources", "derived_sources"):
        for source in contract.get(group, []):
            path = ROOT / source["path"]
            if not path.is_file():
                continue
            actual = sha256_file(path)
            if actual != source["sha256"]:
                changed.append({"id": source["id"], "from": source["sha256"], "to": actual})
                source["sha256"] = actual
    if changed:
        CONTRACT_PATH.write_text(json.dumps(contract, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
    return {"changed": changed, "count": len(changed)}


def self_test() -> dict:
    """基于 example 做规则变异自检；返回每条规则是否按预期拦截。"""
    contract = load_json(CONTRACT_PATH)
    schema = load_json(SCHEMA_PATH)
    base = load_json(EXAMPLE_PATH)
    asset_ids, _ = external_asset_ids()
    cases: list[tuple[str, Any, str]] = []

    def run(name: str, mutate, needle: str) -> None:
        doc = copy.deepcopy(base)
        mutate(doc)
        errs, _ = validate_ir(doc, contract, schema, asset_ids)
        hit = any(needle in item for item in errs)
        cases.append((name, hit, needle))

    run("schema 版本错误", lambda d: d.update(schema="zengine.scene-ir/v0"), "IR schema 与契约不一致")
    run("id 重复", lambda d: d["nodes"][1].update(id="ground_slab"), "id 重复")
    run("低置信度未降级", lambda d: d["nodes"][1].update(confidence=0.2), "必须降级为 kind=unknown")
    run("unknown 缺 reason", lambda d: d["nodes"][-1].pop("reason"), "unknown 节点必须写 reason")
    run("unknown 带确定性字段", lambda d: d["nodes"][-1].update(anchor=[0, 0, 0]), "unknown 节点禁止确定性字段")
    run("缺 provenance", lambda d: d["nodes"][1].pop("provenance"), "必须带 provenance")
    run("provenance 缺 license", lambda d: d["nodes"][1]["provenance"].pop("license"), "provenance 缺 license")
    run("frozen 带 asset_id", lambda d: d["nodes"][0].update(asset_id="star_pillar"), "frozen 节点禁止带 asset_id")
    run("asset 缺 mesh_sha256", lambda d: d["nodes"][1].pop("mesh_sha256"), "必须带 mesh_sha256")
    run("anchor 非有限", lambda d: d["nodes"][1].update(anchor=[0, float("nan"), 0]), "含非有限数值")
    run("scale 非正", lambda d: d["nodes"][1].update(scale=0.0), "scale 必须 > 0")
    run("asset_id 不在白名单", lambda d: d["nodes"][1].update(asset_id="not_exist"), "不在 external_mesh.manifest 白名单")
    run("character 缺 rig", lambda d: d["nodes"][2].pop("rig"), "character 节点必须带 rig")
    run("vfx 缺 timeline", lambda d: d["nodes"][3].pop("timeline"), "vfx_emitter 节点必须带 timeline")
    run("非法 lod", lambda d: d["nodes"][1].update(lod="Ultra"), "不在 enum")

    failed = [name for name, hit, _ in cases if not hit]
    return {"cases": len(cases), "passed": len(cases) - len(failed), "failed": failed,
            "ok": not failed}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="默认动作：校验契约 + IR 实例")
    parser.add_argument("--ir", default=str(EXAMPLE_PATH), help="待校验的 Scene IR 文件")
    parser.add_argument("--strict", action="store_true", help="镜像与 canonical 不一致视为失败")
    parser.add_argument("--refresh-sources", action="store_true", help="显式重算真源 sha 锁并写回 contract.json")
    parser.add_argument("--self-test", action="store_true", help="规则变异自检")
    parser.add_argument("--report", default="", help="额外把 JSON 报告写到该路径")
    args = parser.parse_args(argv)

    if args.refresh_sources:
        report: dict[str, Any] = refresh_sources()
    elif args.self_test:
        report = self_test()
    else:
        report = run_check(Path(args.ir), args.strict)
    if args.report:
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
    ok = report.get("ok", args.refresh_sources)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
