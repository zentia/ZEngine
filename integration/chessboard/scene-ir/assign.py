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

拼现有资产 vs 生成新资产 vs 环境视觉层
----------------------------------------
- 「拼现有资产」：label 命中映射表 -> 直接用 manifest 里的 asset_id（确定性）。
- 「生成新资产」：label 未命中映射表、且是单体实体（structure/prop 或非环境语义
  的 ground）-> 标记 `needs_generation`，留接口给图生 3D provider。
- 「环境视觉层」：label 未命中、且语义是天空/地面/远景/材质（连续铺展的环境），
  -> 标记 `visual_layer`，真源是 visual-pack.json（天空盒/材质/Volume），
  **不是**混元图生 3D 的单体 glb。这是 contract.asset_kinds.scene 的 l2_output
  里「全景贴图 + 视觉参数包」与「glb 单体」两类产物的区分。

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
from decompose import back_project_ground, estimate_world_height  # noqa: E402

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

# 环境视觉层语义关键词（走 visual-pack.json 的天空盒 / 地面材质 / 远景 / 后处理，
# **不是** 混元图生 3D 的单体 glb）。这些语义在 contract.asset_kinds.scene 的
# l2_output 里对应「全景贴图 + 视觉参数包」，而非「glb 单体」。
# 命中即标记 `visual_layer`，与 `needs_generation`（单体实体图生 3D）区分开。
#
# 分两档，避免误伤真正的单体实体：
#   - 强信号（材质/贴花词）：无论 kind 是什么都判环境层——「装饰线」「贴花」
#     「材质」这类词本身就是非实体材质，即使 L1 把它归到 prop（decoration）
#     也仍是地面贴花，不是可摆放的网格。
#   - 弱信号（天空/地面/远景词）：只在 ground kind 下才判环境层——这类词
#     出现在 prop/structure 上时通常是「位置修饰」（如「地面雕像」的「地面」），
#     实体本身仍是单体（「雕像」），不能误判成环境。
VISUAL_LAYER_STRONG: Tuple[str, ...] = (
    "装饰线", "贴花", "贴图", "材质", "纹理",     # 非实体材质 / 贴花
    "辉光", "光晕", "雾", "氛围",                 # 后处理 / 氛围（Volume）
)
VISUAL_LAYER_GROUND: Tuple[str, ...] = (
    "天空", "夜空", "星空", "云",                 # 天空盒
    "地面", "地板", "石板", "路面", "沙", "草地", "水面", "泥土", "地砖",  # 地面材质
    "远山", "山脉", "远景", "剪影", "背景", "山",  # 远景（天空盒 / 背景板）
)


def is_visual_layer(label: str, kind: str) -> bool:
    """判断语义节点属于「环境视觉层」还是「单体实体」。

    两档判断（见 VISUAL_LAYER_STRONG / VISUAL_LAYER_GROUND 注释）：
      - 强信号（材质/贴花词）：任何 kind 都判环境层。
      - 弱信号（天空/地面/远景词）：仅 ground kind 判环境层。
      - structure/prop 上的弱信号词按「位置修饰」处理，实体仍是单体，不误判。
    """
    text = label.lower()
    if any(kw in text for kw in VISUAL_LAYER_STRONG):
        return True
    if kind != "ground":
        return False
    return any(kw in text for kw in VISUAL_LAYER_GROUND)


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
        # 未命中现有资产库 -> 区分「环境视觉层」与「单体实体」：
        #   - 环境视觉层（天空/地面/远景/材质，走 visual-pack.json）标 visual_layer；
        #   - 单体实体（月亮/平台等可摆放物，走混元图生 3D）标 needs_generation。
        if is_visual_layer(label, kind):
            node = dict(node)
            node["assignment"] = {
                "status": "visual_layer",
                "reason": "label %r 是环境视觉层语义（天空/地面/远景/材质），"
                          "真源是 visual-pack.json，不是单体 glb" % label,
            }
            return node
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

    bbox = node.get("bbox_px")
    has_bbox = bbox and len(bbox) == 4

    # anchor：有相机参数才反投影，缺相机留空（诚实边界，不硬算 world）
    if has_bbox and camera is not None:
        proj = back_project_ground(bbox, camera, image_size)
        if proj is not None:
            out["anchor"] = proj["anchor"]
            out["anchor_estimate"] = True

    # scale：有相机 + bbox + 资产 bounds 时，用「物体直立地面」假设反推世界
    # 高度，再除以资产原始高度得 scale。缺任一要素则 scale 保持默认 1.0 并
    # 标 scale_pending（单视角逆渲染欠定，不硬猜）。
    scale = 1.0
    scale_pending = True
    asset_height = _asset_height(asset)
    if has_bbox and camera is not None and asset_height is not None:
        world_h = estimate_world_height(bbox, camera, image_size)
        if world_h is not None and world_h > 0:
            s = world_h / asset_height
            # 数值保护：尺度必须在合理区间，异常值（>100x 或 <0.01x）视为欠定
            if 0.01 <= s <= 100.0:
                scale = round(s, 4)
                scale_pending = False
    out["scale"] = scale
    out["scale_pending"] = scale_pending

    out["assignment"] = {
        "status": "assigned",
        "asset_id": asset_id,
        "matched_keyword": _matched_keyword(label),
    }
    return out


def _asset_height(asset: Dict[str, Any]) -> Optional[float]:
    """从资产 bounds 取世界高度（Y 方向跨度）；无 bounds 返回 None。"""
    bounds = asset.get("bounds")
    if not isinstance(bounds, dict):
        return None
    mn = bounds.get("min")
    mx = bounds.get("max")
    if not isinstance(mn, (list, tuple)) or not isinstance(mx, (list, tuple)):
        return None
    if len(mn) < 2 or len(mx) < 2:
        return None
    h = float(mx[1]) - float(mn[1])
    return h if h > 0 else None


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


def ir_to_placements(assigned_ir: Dict[str, Any]) -> List[Dict[str, Any]]:
    """把 assigned scene-ir 的节点转成 L3 `build_restored_scene` 消费的 placements。

    只转换 `assignment.status == "assigned"` 的节点（已成功拼到现有资产）；
    needs_generation（单体实体图生 3D）/ visual_layer（环境视觉层）/
    skipped 节点跳过（它们没有 asset_id，无法摆放）。

    诚实默认（单视角检测不到的字段不硬猜）：
      - instance_id   = 节点 id（唯一标识）
      - anchor        = 反投影结果（有相机时）；缺相机用 [0,0,0] 占位并标 anchor_pending
      - rotation_y_deg = 0.0（单视角检测不出朝向）
      - scale          = 反推的世界尺度（有相机+bbox+bounds 时）；否则 1.0 并标 scale_pending
      - material       = ""（用资产默认材质）
    """
    placements: List[Dict[str, Any]] = []
    for node in assigned_ir.get("nodes", []):
        if node.get("assignment", {}).get("status") != "assigned":
            continue
        if "asset_id" not in node:
            continue
        anchor = node.get("anchor")
        anchor_pending = anchor is None
        scale = node.get("scale", 1.0)
        scale_pending = node.get("scale_pending", True)
        placement = {
            "asset_id": node["asset_id"],
            "instance_id": node.get("id", ""),
            "anchor": anchor if anchor is not None else [0.0, 0.0, 0.0],
            "rotation_y_deg": 0.0,
            "scale": scale,
            "material": "",
            "tag": "",
            "asset_ready": True,
            "anchor_pending": anchor_pending,
            "scale_pending": scale_pending,
            "source_node_id": node.get("id", ""),
        }
        placements.append(placement)
    return placements


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L2 编排：语义节点 -> 资产指派")
    parser.add_argument("--ir", required=True, help="L1 产出的 scene-ir.json")
    parser.add_argument("--manifest", required=True, help="资产清单 manifest")
    parser.add_argument("--out", required=True, help="输出 assigned scene-ir.json")
    parser.add_argument("--placements-out", default="",
                        help="可选：同时输出 L3 可消费的 placements JSON")
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

    # 可选：输出 placements（L2 -> L3 桥接产物）
    if args.placements_out:
        placements = ir_to_placements(result)
        os.makedirs(os.path.dirname(args.placements_out) or ".", exist_ok=True)
        with open(args.placements_out, "w", encoding="utf-8", newline="\n") as handle:
            json.dump({"placements": placements}, handle, ensure_ascii=False, indent=2)
        print("wrote placements -> %s (%d 个)" % (args.placements_out, len(placements)))

    # 汇总
    assigned = sum(1 for n in result["nodes"]
                   if n.get("assignment", {}).get("status") == "assigned")
    need_gen = sum(1 for n in result["nodes"]
                   if n.get("assignment", {}).get("status") == "needs_generation")
    visual = sum(1 for n in result["nodes"]
                 if n.get("assignment", {}).get("status") == "visual_layer")
    skipped = sum(1 for n in result["nodes"]
                  if n.get("assignment", {}).get("status") == "skipped")
    print("assigned=%d needs_generation=%d visual_layer=%d skipped=%d"
          % (assigned, need_gen, visual, skipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
