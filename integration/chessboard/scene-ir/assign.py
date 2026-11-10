"""L2 资产生成编排层：把 L1 的 scene-ir（语义节点）装配成带 asset_id 的形态。

职责边界（与 contract.ai_boundary 一致）
----------------------------------------
- L1 产出「语义节点」：只有 kind + label + bbox + confidence + depth_rank，
  没有 asset_id（L1 只做语义检测，不决定用哪个具体资产）。
- 本模块（L2 编排层，确定性）负责把语义节点**指派**到具体资产：
    1. 资产指派：kind + label 语义 -> asset_id（确定性映射，拼现有资产）；
    2. anchor：有相机参数时反投影 bbox 底面中心到地面，无相机留空（诚实）；
    3. scale：有资产 bounds + bbox 时反推归一尺度，无则留空；
    4. 产出的节点带 asset_id + mesh_sha256 + pivot_offset，喂给 L3 binder。
- **AI 不碰坐标/变换/asset_id 指派**：这是确定性代码的职责，AI（图生 3D）
  只在「现有资产库没有匹配」时被调用去生成新资产，且生成的 glb 仍由
  `external_mesh.py --import-glb` 确定性接管。

拼现有资产 vs 生成新资产
------------------------
- 「拼现有资产」：label 命中映射表 -> 直接用 manifest 里的 asset_id（确定性）。
- 「生成新资产」：label 未命中映射表 -> 标记 `needs_generation`，留接口给
  图生 3D provider（本轮不接，与 L1 视觉理解同样的可插拔模式）。

用法
----
    python scene-ir/assign.py --ir build/scene-ir.json \
        --manifest build/external_mesh.manifest.json \
        --out build/scene-ir.assigned.json [--camera-json cam.json]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import decompose  # noqa: E402
from decompose import back_project_ground  # noqa: E402

# kind -> 可指派资产的 kind 集合。只有这三种场景类 kind 参与资产指派；
# character / vfx_emitter 在 L1 已降级 unknown，L2 不再碰（它们的必需字段
# rig/emitter/timeline 是更上层生成器的职责，见 contract.asset_kinds）。
ASSIGNABLE_KINDS = frozenset({"ground", "structure", "prop"})


# 语义 label -> asset_id 的确定性映射（拼现有资产）。
# key 是 label 里会出现的关键词（小写匹配），value 是 manifest 里的 asset_id。
# 顺序有意义：先匹配更具体的词（如「灯柱」「圆环」），再匹配泛化的词。
LABEL_TO_ASSET: List[Tuple[str, str]] = [
    ("灯柱", "star_pillar"),      # 星象灯柱
    ("星象", "star_pillar"),      # 星象灯柱（兜底）
    ("圆环", "halo_ring"),        # 悬浮星象圆环
    ("悬浮", "halo_ring"),        # 悬浮星象圆环
    ("光环", "halo_ring"),        # 地面光环 -> 圆环
    ("星象仪", "moon_orrery"),    # 月相星象仪
    ("月相", "moon_orrery"),      # 月相星象仪
    ("星灯", "star_lantern"),     # 可点击星灯
    ("灯笼", "star_lantern"),     # 灯笼 -> 星灯（金铲铲的三盏灯）
    ("灯", "star_lantern"),       # 灯（泛化兜底）
]


def match_asset(label: str, available_ids: List[str]) -> Optional[str]:
    """按语义关键词把 label 匹配到 asset_id；未命中返回 None（不硬猜）。"""
    text = label.lower()
    for keyword, asset_id in LABEL_TO_ASSET:
        if keyword in text and asset_id in available_ids:
            return asset_id
    return None


def assign_node(node: Dict[str, Any], manifest: Dict[str, Any],
                camera: Optional[Dict[str, Any]],
                image_size: Tuple[int, int]) -> Dict[str, Any]:
    """给单个语义节点指派资产（确定性，不调模型）。

    返回新节点（不修改入参）。指派失败/不适用时保持原节点语义，附加
    `assignment` 字段说明原因（诚实边界：不硬猜）。
    """
    kind = node.get("kind", "unknown")
    if kind not in ASSIGNABLE_KINDS:
        # character / vfx_emitter / unknown：不参与资产指派
        node = dict(node)
        node["assignment"] = {"status": "skipped", "reason": "kind 不参与资产指派"}
        return node

    assets = manifest.get("assets", [])
    available_ids = [a["asset_id"] for a in assets]
    label = node.get("label", "")

    asset_id = match_asset(label, available_ids)
    if asset_id is None:
        # 现有资产库没有匹配 -> 标记需生成新资产（图生 3D 接口，本轮不接）
        node = dict(node)
        node["assignment"] = {
            "status": "needs_generation",
            "reason": "label %r 未命中现有资产库，需图生 3D 或人工指派" % label,
        }
        return node

    # 找到匹配资产，读取其元数据
    asset = next(a for a in assets if a["asset_id"] == asset_id)
    out = dict(node)
    out["asset_id"] = asset_id
    out["mesh_sha256"] = asset.get("mesh_sha256")
    out["pivot_offset"] = asset.get("pivot_offset", [0.0, 0.0, 0.0])

    # anchor：有相机参数才反投影，缺相机留空（诚实边界，不硬算 world）
    bbox = node.get("bbox_px")
    if bbox and camera is not None and len(bbox) == 4:
        proj = back_project_ground(bbox, camera, image_size)
        if proj is not None:
            out["anchor"] = proj["anchor"]
            out["anchor_estimate"] = True

    # scale：有资产 bounds + bbox 时反推；本轮先不自动算（单视角 bbox 高度
    # 反推 scale 欠定，留 L3 用资产默认尺度），仅记录待定。
    out["scale_pending"] = True

    out["assignment"] = {
        "status": "assigned",
        "asset_id": asset_id,
        "matched_keyword": _matched_keyword(label),
    }
    return out


def _matched_keyword(label: str) -> str:
    text = label.lower()
    for keyword, _ in LABEL_TO_ASSET:
        if keyword in text:
            return keyword
    return ""


def assign_assets(scene_ir: Dict[str, Any], manifest: Dict[str, Any],
                  camera: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """把 scene-ir 的语义节点装配成带 asset_id 的形态（L2 编排主入口）。

    返回新的 scene-ir dict，nodes 里每个可指派节点被替换为带 asset_id 的版本；
    unknown / character / vfx 节点原样保留（附 assignment=skipped 说明）。
    """
    image_size = _image_size_from_ir(scene_ir)
    new_nodes = []
    for node in scene_ir.get("nodes", []):
        new_nodes.append(assign_node(node, manifest, camera, image_size))

    out = dict(scene_ir)
    out["nodes"] = new_nodes
    return out


def _image_size_from_ir(scene_ir: Dict[str, Any]) -> Tuple[int, int]:
    """从 scene-ir 里尽量还原图像尺寸（反投影用）；缺省回退 1920x1080。"""
    # 优先从 capture_profiles / camera 里找，找不到就回退
    src = scene_ir.get("source", {})
    # 本骨架暂不存图像尺寸，回退占位（反投影只在有 camera 时用，占位不影响
    # 无相机时的诚实降级）
    return (1920, 1080)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L2 编排：语义节点 -> 资产指派")
    parser.add_argument("--ir", required=True, help="L1 产出的 scene-ir.json")
    parser.add_argument("--manifest", required=True, help="资产清单 manifest")
    parser.add_argument("--out", required=True, help="输出 assigned scene-ir.json")
    parser.add_argument("--camera-json", default="", help="可选相机参数 JSON")
    args = parser.parse_args(argv)

    with open(args.ir, "r", encoding="utf-8") as handle:
        scene_ir = json.load(handle)
    with open(args.manifest, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)

    camera = None
    if args.camera_json:
        with open(args.camera_json, "r", encoding="utf-8") as handle:
            camera = json.load(handle)
    elif scene_ir.get("source", {}).get("camera"):
        camera = scene_ir["source"]["camera"]

    result = assign_assets(scene_ir, manifest, camera=camera)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=2)
    print("wrote assigned scene-ir -> %s" % args.out)

    # 汇总
    assigned = sum(1 for n in result["nodes"]
                   if n.get("assignment", {}).get("status") == "assigned")
    need_gen = sum(1 for n in result["nodes"]
                   if n.get("assignment", {}).get("status") == "needs_generation")
    skipped = sum(1 for n in result["nodes"]
                  if n.get("assignment", {}).get("status") == "skipped")
    print("assigned=%d needs_generation=%d skipped=%d"
          % (assigned, need_gen, skipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
