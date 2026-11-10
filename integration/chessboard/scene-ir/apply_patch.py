"""L5 局部 IR 补丁的确定性应用层。

职责边界（与 contract.ai_boundary 一致）
----------------------------------------
- AI 负责「建议改哪个节点、改什么」（写 patch-suggestion.json，见
  patch-suggestion.schema.json）；确定性代码（本模块）负责校验与应用：
    1. 校验补丁契约（字段白名单、op 语义、数值类型）；
    2. 拒绝改写 frozen 节点 / 身份字段（id/kind/confidence/provenance）；
    3. 应用 set/delete/add 到 assigned scene-ir；
    4. 应用后重跑 verify_scene_ir.validate_ir，确保补丁不破坏契约；
    5. 输出 patched scene-ir + 应用报告（applied/rejected 逐条），供 L4 回归。

补丁字段白名单（对应 schema 的摆放/外观字段，不含身份与合规字段）：
    anchor / rotation_y_deg / scale / asset_id / pivot_offset /
    material / tag / lod

frozen 判定（拒绝改写）：
    - node.frozen == true（连续台面 / 包边 / 棋手占位 / 逻辑网格）
    - node.assignment.status == "frozen"（L2 判定的 core_slab 棋盘本体）
    - node.geometry_ref 指向 board.zbrd 冻结几何

用法
----
    python scene-ir/apply_patch.py --ir build/scene-ir.assigned.json \
        --patch build/patch-suggestion.json \
        --out build/scene-ir.patched.json \
        [--report-out build/patch_report.json] [--check]

    --check：应用后重跑 verify_scene_ir 校验（errors 非零则返回非 0）。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import verify_scene_ir  # noqa: E402

# 补丁可改字段白名单：只允许摆放 / 外观字段。身份与合规字段（id/kind/
# confidence/provenance/geometry_ref/frozen）一律拒绝。
PATCHABLE_FIELDS = frozenset({
    "anchor", "rotation_y_deg", "scale", "asset_id",
    "pivot_offset", "material", "tag", "lod",
})

# 字段 -> 期望类型（用于数值校验）。anchor/pivot_offset 是 3 元素 number 数组。
VEC3_FIELDS = frozenset({"anchor", "pivot_offset"})
NUM_FIELDS = frozenset({"rotation_y_deg", "scale"})
STR_FIELDS = frozenset({"asset_id", "material", "tag", "lod"})

LOD_VALUES = frozenset({"HD", "High", "Mid", "Low"})

PATCH_SCHEMA = "zengine.patch-suggestion/v1"


def _is_frozen(node: Dict[str, Any]) -> bool:
    """判断节点是否冻结几何（补丁禁止改写）。"""
    if node.get("frozen") is True:
        return True
    assignment = node.get("assignment")
    if isinstance(assignment, dict) and assignment.get("status") == "frozen":
        return True
    if node.get("geometry_ref"):
        return True
    return False


def _is_identity_field(field: str) -> bool:
    """身份 / 合规字段，禁止通过补丁改写。"""
    return field in frozenset({
        "id", "kind", "confidence", "provenance", "geometry_ref",
        "frozen", "assignment", "bbox_px", "reason", "label",
    })


def _validate_value(field: str, value: Any) -> Optional[str]:
    """校验补丁目标值类型；返回错误描述，None 表示合法。"""
    if field in VEC3_FIELDS:
        if not isinstance(value, (list, tuple)) or len(value) != 3:
            return f"{field} 必须是 3 元素 number 数组，实际 {value!r}"
        if not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in value):
            return f"{field} 数组元素必须是 number，实际 {value!r}"
        return None
    if field in NUM_FIELDS:
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return f"{field} 必须是 number，实际 {value!r}"
        if field == "scale" and value < 0.0:
            return f"scale 必须 >= 0，实际 {value!r}"
        return None
    if field in STR_FIELDS:
        if not isinstance(value, str):
            return f"{field} 必须是 string，实际 {value!r}"
        if field == "lod" and value not in LOD_VALUES:
            return f"lod 必须在 {sorted(LOD_VALUES)!r} 内，实际 {value!r}"
        return None
    return f"未知字段 {field!r}（不在白名单）"


def _find_node(nodes: List[Dict[str, Any]], node_id: str) -> Tuple[int, Optional[Dict[str, Any]]]:
    for i, n in enumerate(nodes):
        if n.get("id") == node_id:
            return i, n
    return -1, None


def validate_patch(patch: Dict[str, Any]) -> List[str]:
    """校验补丁契约（shape 之外的业务规则，shape 已由 schema 保证）。

    返回错误列表；空列表表示契约合法（可进入 apply 阶段）。
    """
    errors: List[str] = []
    if patch.get("schema") != PATCH_SCHEMA:
        errors.append(f"schema 必须是 {PATCH_SCHEMA!r}")

    for i, p in enumerate(patch.get("patches", [])):
        where = f"patches[{i}]"
        op = p.get("op")
        node_id = p.get("node_id")
        field = p.get("field")

        if not isinstance(node_id, str) or not node_id:
            errors.append(f"{where}: node_id 缺失或非 string")

        if op == "set":
            if field not in PATCHABLE_FIELDS:
                errors.append(f"{where}: field {field!r} 不在补丁白名单")
            elif _is_identity_field(field):
                errors.append(f"{where}: field {field!r} 是身份/合规字段，禁止补丁改写")
            else:
                err = _validate_value(field, p.get("value"))
                if err:
                    errors.append(f"{where}: {err}")
        elif op == "delete":
            # delete 不需要 field/value
            if "field" in p or "value" in p:
                errors.append(f"{where}: delete 不应带 field/value")
        elif op == "add":
            node = p.get("node")
            if not isinstance(node, dict):
                errors.append(f"{where}: add 必须带合法 node 对象")
            elif node.get("id") != node_id:
                errors.append(f"{where}: add 的 node.id {node.get('id')!r} 与 node_id {node_id!r} 不一致")
            elif "kind" not in node or "confidence" not in node:
                errors.append(f"{where}: add 的 node 必须带 kind + confidence")
        else:
            errors.append(f"{where}: 未知 op {op!r}（只支持 set/delete/add）")

    return errors


def apply_patch(ir: Dict[str, Any], patch: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    """应用补丁到 assigned scene-ir，返回 (patched_ir, report)。

    每条 patch 逐条应用，产出 applied / rejected 两类结果；rejected 不中断
    其余补丁（局部补丁允许部分失败，失败项由 report 显式标出供 AI 修正）。
    """
    out = dict(ir)
    nodes: List[Dict[str, Any]] = [dict(n) for n in out.get("nodes", [])]
    out["nodes"] = nodes

    applied: List[Dict[str, Any]] = []
    rejected: List[Dict[str, Any]] = []

    for i, p in enumerate(patch.get("patches", [])):
        op = p.get("op")
        node_id = p.get("node_id")
        where = f"patches[{i}]"

        if op == "add":
            node = dict(p["node"])
            # 新增节点不能与现有 id 冲突
            if any(n.get("id") == node_id for n in nodes):
                rejected.append({
                    "index": i, "node_id": node_id, "op": op,
                    "reason": f"节点 {node_id!r} 已存在，add 冲突",
                })
                continue
            nodes.append(node)
            applied.append({"index": i, "node_id": node_id, "op": op})
            continue

        idx, node = _find_node(nodes, node_id)
        if node is None:
            rejected.append({
                "index": i, "node_id": node_id, "op": op,
                "reason": f"节点 {node_id!r} 不存在",
            })
            continue

        if _is_frozen(node):
            rejected.append({
                "index": i, "node_id": node_id, "op": op,
                "reason": f"节点 {node_id!r} 是冻结几何（frozen/geometry_ref），禁止改写",
            })
            continue

        if op == "set":
            field = p["field"]
            value = p["value"]
            if field in VEC3_FIELDS:
                value = [float(v) for v in value]
            elif field in NUM_FIELDS:
                value = float(value)
            old = node.get(field)
            nodes[idx][field] = value
            applied.append({
                "index": i, "node_id": node_id, "op": op, "field": field,
                "from": old, "to": value,
            })
        elif op == "delete":
            del nodes[idx]
            applied.append({"index": i, "node_id": node_id, "op": op})

    report: Dict[str, Any] = {
        "schema": "zengine.patch-report/v1",
        "applied_count": len(applied),
        "rejected_count": len(rejected),
        "applied": applied,
        "rejected": rejected,
    }
    return out, report


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L5 补丁应用：patch-suggestion -> patched scene-ir")
    parser.add_argument("--ir", required=True, help="assigned scene-ir.json")
    parser.add_argument("--patch", required=True, help="patch-suggestion.json")
    parser.add_argument("--out", required=True, help="输出 patched scene-ir.json")
    parser.add_argument("--report-out", default="", help="可选：应用报告 JSON 路径")
    parser.add_argument("--check", action="store_true",
                        help="应用后重跑 verify_scene_ir 校验")
    args = parser.parse_args(argv)

    with open(args.ir, "r", encoding="utf-8") as handle:
        ir = json.load(handle)
    with open(args.patch, "r", encoding="utf-8") as handle:
        patch = json.load(handle)

    errors = validate_patch(patch)
    if errors:
        print("补丁契约校验失败：")
        for e in errors:
            print("  [E] " + e)
        return 2

    patched, report = apply_patch(ir, patch)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(patched, handle, ensure_ascii=False, indent=2)
    print("wrote patched scene-ir -> %s" % args.out)

    if args.report_out:
        os.makedirs(os.path.dirname(args.report_out) or ".", exist_ok=True)
        with open(args.report_out, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
        print("wrote patch report -> %s" % args.report_out)

    print("applied=%d rejected=%d"
          % (report["applied_count"], report["rejected_count"]))
    for r in report["rejected"]:
        print("  [R] %s" % r["reason"])

    if args.check:
        contract = verify_scene_ir.load_json(verify_scene_ir.CONTRACT_PATH)
        schema = verify_scene_ir.load_json(verify_scene_ir.SCHEMA_PATH)
        asset_ids, _ = verify_scene_ir.external_asset_ids()
        # L2 中间字段（assignment / scale_pending / anchor_estimate）不属于
        # scene-ir schema，assign.py 内部使用；校验前剥离，只校验补丁后 IR
        # 的 scene-ir 合法部分。assigned IR 本就不满足 schema（additionalProperties
        # false），不能拿它直接 validate_ir。
        ir_for_check = _strip_l2_fields(patched)
        ir_errors, ir_warnings = verify_scene_ir.validate_ir(
            ir_for_check, contract, schema, asset_ids)
        print("patched errors: %d, warnings: %d" % (len(ir_errors), len(ir_warnings)))
        for e in ir_errors:
            print("  [E] " + e)
        for w in ir_warnings:
            print("  [W] " + w)
        if ir_errors:
            return 3
    return 0


def _strip_l2_fields(ir: Dict[str, Any]) -> Dict[str, Any]:
    """剥离 L2 中间字段，返回 scene-ir 合法子集（供 --check 校验）。"""
    stripped = dict(ir)
    nodes = []
    for n in stripped.get("nodes", []):
        m = dict(n)
        for key in ("assignment", "scale_pending", "anchor_estimate"):
            m.pop(key, None)
        nodes.append(m)
    stripped["nodes"] = nodes
    return stripped


if __name__ == "__main__":
    raise SystemExit(main())
