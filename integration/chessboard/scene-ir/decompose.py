"""L1 分解层的确定性装配骨架：把 AI 模型的原始检测结果装配成 scene-ir.json。

职责边界（与 contract.json 的 ai_boundary 一致）
------------------------------------------------
- AI 模型负责：打标、实例分割、单目深度、层序、物体归类。模型产出的原始
  检测结果用 `Detection` 表达（bbox 像素 + 类别 + 置信度 + 可选深度）。
- 确定性代码（本模块）负责：类别 -> kind 映射、置信度阈值 + unknown 降级、
  bbox 归一化 / quad 计算、深度排序、反投影（有相机参数时估 anchor）。

本模块**不调用任何真实模型**：模型侧留成可插拔接口（`Detection` 列表进来），
先拿 mock 数据跑通「检测结果 -> 合法 scene-ir.json」的全链路，再接混元 /
腾讯内部多模态服务。这样 L1 的确定性部分可离线单测、可复现，不受模型
黑盒影响。

反投影口径（诚实边界）
----------------------
单张截图 + 无相机参数时，bbox 只能留在像素坐标，**不硬算 world anchor**
（单视角逆渲染欠定，正是需求文档第九节「不承诺 1:1 复刻」的原因）。只有
source.camera 提供 fov / pitch / position / target 时，才做「bbox 底面中心
-> 地面平面交点」的 anchor 估算，且仍标 `estimate`（非 ground truth）。

用法
----
    python scene-ir/decompose.py --detections build/l1_mock_detections.json \
        --out build/scene-ir.json [--camera ...]

单元调用（供测试 / 后续接模型）：
    from scene-ir.decompose import Detection, decompose_to_ir
    ir = decompose_to_ir([Detection(...)], source_sha256="...")
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import verify_scene_ir  # noqa: E402

# 类别 -> kind 映射（模型语义类别 -> scene-ir kind）。未知类别归 unknown。
# 这是确定性代码负责的「归类」：不允许模型直接吐 kind，只允许吐类别标签，
# 由这里按白名单映射，未命中一律 unknown（对应"不要自己意淫"）。
CATEGORY_TO_KIND: Dict[str, str] = {
    "ground": "ground",
    "floor": "ground",
    "slab": "ground",
    "board": "ground",
    "structure": "structure",
    "pillar": "structure",
    "wall": "structure",
    "frame": "structure",
    "prop": "prop",
    "decoration": "prop",
    "lantern": "prop",
    "orb": "prop",
    "furniture": "prop",
    "character": "character",
    "hero": "character",
    "monster": "character",
    "pet": "character",
    "vfx": "vfx_emitter",
    "particle": "vfx_emitter",
    "effect": "vfx_emitter",
    "camera": "camera",
}


@dataclass
class Detection:
    """AI 模型产出的单个检测结果（模型无关的归一形态）。

    bbox: [x0, y0, x1, y1] 像素坐标（左上 -> 右下，闭区间语义不限）。
    category: 模型语义类别标签（字符串，非 scene-ir kind）。
    confidence: [0,1]。
    depth_rank: 可选，相对层序（0=最远 -> N=最近）。这是 VLM 单目唯一可信
        的深度信号（详见 eval_depth.py 的结论：绝对米不可信，相对层序可信）。
    depth_m: 可选，绝对深度（米）。**不可信**，仅当有真实相机/深度源时才
        有意义；VLM 单目输出时不要填，用 depth_rank 代替。
    label: 可选，人读标签（如 "朱红鼓车"）。
    mask: 可选，实例分割 mask 的引用（本骨架只透传 id，不做像素处理）。
    node_id: 可选，该检测对应的 scene-ir 节点 id。这是 L4 对拍下沉到 node
        级归因的稳定锚点：基准帧与渲染帧两侧用同一个 node_id 才能精确配对
        （否则退回 bbox IoU 贪心匹配）。注意：decompose_to_ir 装配时生成的
        `kind_idx` id 依赖排序顺序，不能可靠回填到排序前的 Detection，故
        node_id 应由上游（模型侧或对拍准备阶段）在检测时显式分配并保持稳定，
        与 scene-ir.json 的 nodes[].id 一致。
    """
    bbox: Sequence[float]
    category: str
    confidence: float
    depth_rank: Optional[float] = None
    depth_m: Optional[float] = None
    label: Optional[str] = None
    mask_id: Optional[str] = None
    node_id: Optional[str] = None

    def __post_init__(self) -> None:
        if len(self.bbox) != 4:
            raise ValueError("bbox 必须是 [x0, y0, x1, y1] 4 个数字")
        if not (0.0 <= self.confidence <= 1.0):
            raise ValueError("confidence 必须落在 [0,1]")


def map_kind(category: str) -> str:
    """类别标签 -> scene-ir kind；未命中返回 unknown。"""
    key = category.strip().lower()
    return CATEGORY_TO_KIND.get(key, "unknown")


# L1 阶段只能从截图检测出「这里有个物体 / 特效 / 人物」，但以下 kind 的
# 必需字段（rig/animation_slots、emitter/timeline）是 L2 生成器才产出的，
# 单张截图无法判定。L1 把这些 kind 降级 unknown，reason 注明待 L2。
L1_DEFERRED_KINDS = frozenset({"character", "vfx_emitter"})


def normalize_bbox(bbox: Sequence[float], width: int, height: int) -> Dict[str, Any]:
    """像素 bbox -> 归一化 quad（相对图像尺寸），并附像素值。

    归一化用 [0,1]（相对宽高），避免不同分辨率截图下 bbox 数值不可比。
    同时返回像素 bbox 供反投影 / 对拍使用。
    """
    x0, y0, x1, y1 = (float(v) for v in bbox)
    w = float(width) if width else 1.0
    h = float(height) if height else 1.0
    return {
        "bbox_px": [x0, y0, x1, y1],
        "quad": [x0 / w, y0 / h, x1 / w, y1 / h],
        "image_size": [width, height],
    }


def back_project_ground(bbox: Sequence[float], camera: Dict[str, Any],
                        image_size: Tuple[int, int]) -> Optional[Dict[str, Any]]:
    """bbox 底面中心 -> 地面平面交点的 anchor 估算（仅当相机参数齐备）。

    相机参数齐备时：沿视线把 bbox 底面中心反投影到 y=0 地面平面，得到
    anchor 的粗略估算。这是"估算"不是 ground truth——单视角逆渲染欠定，
    尺寸 / 深度都不可唯一解出，所以返回的 anchor 只作初值，带
    `estimate: true`，且 scale 由 L2 资产归一时再定，这里不硬填。

    相机参数缺失时返回 None（调用方应只在 bbox 留像素坐标，不写 world）。
    """
    required = ("fov", "pitch_deg", "position")
    if not isinstance(camera, dict) or any(camera.get(k) is None for k in required):
        return None
    try:
        fov = float(camera["fov"])
        pitch = math.radians(float(camera["pitch_deg"]))
        pos = [float(v) for v in camera["position"]]
        target = camera.get("target")
        target = [float(v) for v in target] if isinstance(target, (list, tuple)) and len(target) == 3 else [0.0, 0.0, 0.0]
    except (TypeError, ValueError):
        return None

    width, height = image_size
    if width <= 0 or height <= 0:
        return None

    # bbox 底面中心（像素 -> 归一化）
    x0, y0, x1, y1 = (float(v) for v in bbox)
    cx = (x0 + x1) / 2.0 / width
    cy = y1 / height  # 底面 = bbox 下边

    # 相机朝 target 的水平朝向（忽略 roll，仅 pitch 俯角 + yaw 由 target 定）
    dx = target[0] - pos[0]
    dz = target[2] - pos[2]
    yaw = math.atan2(dx, dz)

    # 视锥：垂直 fov 决定纵向张角；cx 决定水平偏移
    aspect = width / height
    vfov = math.radians(fov)
    hfov = 2.0 * math.atan(math.tan(vfov / 2.0) * aspect)

    # 从屏幕中心偏移到目标点的角度
    off_x = (cx - 0.5) * hfov
    off_y = (cy - 0.5) * vfov

    # 世界方向（先 pitch 俯角，再叠加屏幕偏移，再绕 yaw）。
    # 俯角向下：视线方向 y 分量为 -sin(v_angle)。与 y=0 平面求交：
    #   pos.y + t * (-sin(v_angle)) = 0  =>  t = pos.y / sin(v_angle)
    h_angle = yaw + off_x
    v_angle = pitch + off_y

    sin_v = math.sin(v_angle)
    if abs(sin_v) < 1e-6:
        return None  # 视线近平行地面，无交点
    t = pos[1] / sin_v
    if t <= 0:
        return None  # 交点在地面之上（视线朝上或相机在地面下）

    world_x = pos[0] + t * math.sin(h_angle) * math.cos(v_angle)
    world_z = pos[2] + t * math.cos(h_angle) * math.cos(v_angle)

    return {
        "anchor": [round(world_x, 4), 0.0, round(world_z, 4)],
        "estimate": True,
        "note": "bbox 底面中心反投影到 y=0 的 anchor 估算（单视角欠定，仅初值）",
    }


def estimate_world_height(bbox: Sequence[float], camera: Dict[str, Any],
                          image_size: Tuple[int, int]) -> Optional[float]:
    """估算 bbox 对应物体的世界高度（米），基于「物体直立在地面」假设。

    原理：bbox 底边反投影到地面得 anchor 与视线距离 t_bottom；bbox 顶边的
    视线与「过 anchor 的竖直线」相交，交点高度即物体世界高度 H。
    这是单视角的确定性近似（非 ground truth）：假设物体直立、无遮挡、
    底边确实贴地。返回 None 表示无法估算（缺相机 / 视线近乎平行地面 /
    bbox 退化）。

    与 back_project_ground 共享同一相机模型（fov/pitch/position/target）。
    """
    required = ("fov", "pitch_deg", "position")
    if not isinstance(camera, dict) or any(camera.get(k) is None for k in required):
        return None
    try:
        fov = float(camera["fov"])
        pitch = math.radians(float(camera["pitch_deg"]))
        pos = [float(v) for v in camera["position"]]
        target = camera.get("target")
        target = [float(v) for v in target] if isinstance(target, (list, tuple)) and len(target) == 3 else [0.0, 0.0, 0.0]
    except (TypeError, ValueError):
        return None

    width, height = image_size
    if width <= 0 or height <= 0:
        return None

    x0, y0, x1, y1 = (float(v) for v in bbox)
    if y1 <= y0 or y0 < 0:
        return None  # bbox 退化或顶边越界

    # 相机朝向
    dx = target[0] - pos[0]
    dz = target[2] - pos[2]
    yaw = math.atan2(dx, dz)
    aspect = width / height
    vfov = math.radians(fov)
    hfov = 2.0 * math.atan(math.tan(vfov / 2.0) * aspect)

    # bbox 底面中心的像素 -> 归一化
    cx = (x0 + x1) / 2.0 / width
    cy_bottom = y1 / height

    # 底面视线方向（用于反投影 anchor 与距离）
    off_x = (cx - 0.5) * hfov
    off_y_bottom = (cy_bottom - 0.5) * vfov
    h_angle = yaw + off_x
    v_angle_bottom = pitch + off_y_bottom

    sin_v = math.sin(v_angle_bottom)
    if abs(sin_v) < 1e-6:
        return None
    t_bottom = pos[1] / sin_v
    if t_bottom <= 0:
        return None

    # anchor 的世界 XZ
    anchor_x = pos[0] + t_bottom * math.sin(h_angle) * math.cos(v_angle_bottom)
    anchor_z = pos[2] + t_bottom * math.cos(h_angle) * math.cos(v_angle_bottom)

    # 顶边视线：同一 cx，但 cy 用顶边 y0
    cy_top = y0 / height
    off_y_top = (cy_top - 0.5) * vfov
    v_angle_top = pitch + off_y_top

    # 顶边视线方向（水平角度不变 h_angle）
    # 方向向量：d = (sin(h_angle)*cos(v_top), -sin(v_top), cos(h_angle)*cos(v_top))
    # 求 λ 使 XZ 到达 anchor：pos + λ*d 的 XZ == anchor 的 XZ
    #   水平分量：cos(v_top)；水平位移 = t_top * cos(v_top)
    #   水平位移需等于 anchor 到相机的地面投影距离
    # 用 y 分量求交：pos.y + λ * (-sin(v_top)) = H => H = pos.y - λ*sin(v_top)
    # 而 λ 由水平位移决定：λ = 水平距离 / cos(v_top)
    # 水平距离 = sqrt((anchor_x-pos[0])^2 + (anchor_z-pos[2])^2)
    cos_top = math.cos(v_angle_top)
    if abs(cos_top) < 1e-6:
        return None
    horiz_dist = math.hypot(anchor_x - pos[0], anchor_z - pos[2])
    lam = horiz_dist / cos_top
    # 物体世界高度 = 顶边视线在 anchor 水平位置处的高度
    H = pos[1] - lam * math.sin(v_angle_top)
    if H < 0:
        return None  # 顶边视线在 anchor 处低于地面（bbox 顶边低于地平线，异常）
    return H


def decompose_to_ir(
    detections: Sequence[Detection],
    source_image_sha256: str,
    camera: Optional[Dict[str, Any]] = None,
    image_size: Tuple[int, int] = (1920, 864),
    threshold: float = 0.5,
    source_views: int = 1,
    origin: str = "hunyuan_vision",
    license_: str = "internal",
    notes: str = "",
) -> Dict[str, Any]:
    """把检测结果列表装配成合法的 scene-ir.json。

    确定性装配步骤（全在本函数，不依赖模型）：
    1. 层序排序（优先 depth_rank 升序 = 0 最远在前；无 rank 时退回 depth_m
       降序；两者皆无保持原序）；
    2. 类别 -> kind 映射（未命中 unknown）；
    3. confidence < threshold -> 降级 unknown + reason；
    4. bbox 归一化 + 反投影（有相机参数时估 anchor）；
    5. 每个非 unknown 节点补 provenance；带 anchor 的节点标 estimate。

    返回 dict；若需校验，调用方再走 verify_scene_ir.validate_ir。
    """
    if not (0.0 < threshold < 1.0):
        raise ValueError("threshold 必须落在 (0,1)")

    # 层序排序：depth_rank 是 VLM 唯一可信的深度信号（相对层序，0=最远）。
    # 有 depth_rank 的按升序排（0 最远在前）；否则退回 depth_m 降序；两者都
    # 没有的排到有深度信号之后（保持原相对顺序）。
    def sort_key(d: Detection) -> Tuple[int, float, float]:
        if d.depth_rank is not None:
            # rank 优先：有 rank 的排在前面（按 rank 升序）
            return (0, float(d.depth_rank), 0.0)
        if d.depth_m is not None:
            # 退 depth_m：按米降序（远的在前），排在有 rank 的之后
            return (1, 0.0, -float(d.depth_m))
        # 都没有：最后，保持原序
        return (2, 0.0, 0.0)

    ordered = sorted(enumerate(detections), key=lambda e: sort_key(e[1]))

    nodes: List[Dict[str, Any]] = []
    seen_ids: set = set()
    for idx, det in ordered:
        kind = map_kind(det.category)
        confidence = det.confidence

        # 低置信度 / 未命中类别 / L1 阶段缺 L2 必需字段的 kind -> 降级 unknown
        deferred = kind in L1_DEFERRED_KINDS
        if kind == "unknown" or confidence < threshold or deferred:
            node_id = _unique_id("unknown", idx, seen_ids)
            # 回填 node_id 到 Detection，使 L4 对拍能拿到同一稳定 id 作锚点
            if det.node_id is None:
                det.node_id = node_id
            node: Dict[str, Any] = {
                "id": node_id,
                "kind": "unknown",
                "confidence": confidence,
                "reason": _unknown_reason(det, kind, threshold, deferred),
            }
            # unknown 仍保留 bbox（像素观测，非确定性 world 字段，schema 允许）
            if len(det.bbox) == 4:
                node["bbox_px"] = [float(v) for v in det.bbox]
            nodes.append(node)
            continue

        node_id = _unique_id(kind, idx, seen_ids)
        if det.node_id is None:
            det.node_id = node_id
        node = {
            "id": node_id,
            "kind": kind,
            "confidence": confidence,
        }
        if det.label:
            node["label"] = det.label
        if len(det.bbox) == 4:
            node["bbox_px"] = [float(v) for v in det.bbox]

        # 反投影：相机齐备才估 anchor，缺相机只留 bbox
        if camera is not None:
            proj = back_project_ground(det.bbox, camera, image_size)
            if proj is not None:
                node["anchor"] = proj["anchor"]

        node["provenance"] = {
            "origin": origin,
            "source_image_sha256": source_image_sha256,
            "license": license_,
        }
        if det.mask_id:
            node["provenance"]["job_id"] = det.mask_id
        nodes.append(node)

    ir: Dict[str, Any] = {
        "schema": "zengine.scene-ir/v1",
        "source": {
            "image_sha256": source_image_sha256,
            "views": source_views,
        },
        "units": "meter",
        "coordinate_system": "glTF Y-up right-handed",
        "nodes": nodes,
    }
    if camera is not None:
        ir["source"]["camera"] = camera
    if notes:
        ir["notes"] = notes
    return ir


def _unique_id(kind: str, idx: int, seen: set) -> str:
    base = f"{kind}_{idx}"
    candidate = base
    n = 1
    while candidate in seen:
        candidate = f"{base}_{n}"
        n += 1
    seen.add(candidate)
    return candidate


def _unknown_reason(det: Detection, kind: str, threshold: float,
                    deferred: bool = False) -> str:
    if kind == "unknown":
        return f"类别 {det.category!r} 不在 scene-ir kind 白名单，无法归类"
    if deferred:
        return f"kind={kind} 的必需字段（rig/emitter/timeline）需 L2 生成器产出，L1 单视角无法判定"
    return f"confidence {det.confidence} < {threshold}，单视角欠定，降级 unknown"


# ---------------------------------------------------------------------------
# mock 数据（供测试与演示，接真模型后删除或由模型输出替代）
# ---------------------------------------------------------------------------

def make_mock_detections() -> List[Detection]:
    """一组模拟金铲铲棋盘截图的检测结果（含分层、高低置信度、未知类别）。

    仅用于跑通确定性装配链路；真实数据来自混元 / 腾讯内部多模态服务。
    注意：mock 用 `depth_rank`（相对层序 0=最远）而非 `depth_m`——这是
    eval_depth.py 验证出的 VLM 真实能力边界（绝对米不可信，层序可信）。
    """
    return [
        Detection(bbox=[0, 0, 1920, 864], category="board",
                  confidence=0.94, depth_rank=0, label="夜空背景"),
        Detection(bbox=[0, 200, 1920, 500], category="ground",
                  confidence=0.90, depth_rank=1, label="远景山脉"),
        Detection(bbox=[120, 300, 420, 620], category="pillar",
                  confidence=0.88, depth_rank=2, label="岩壁灯列"),
        Detection(bbox=[1500, 280, 1780, 600], category="lantern",
                  confidence=0.81, depth_rank=3, label="朱红鼓车"),
        Detection(bbox=[700, 320, 1220, 560], category="prop",
                  confidence=0.66, depth_rank=4, label="云纹浅台"),
        Detection(bbox=[300, 200, 500, 400], category="character",
                  confidence=0.42, depth_rank=5, label="人物(低置信)"),
        Detection(bbox=[800, 500, 900, 560], category="something_unknown",
                  confidence=0.90, depth_rank=6, label="无法归类物体"),
        Detection(bbox=[0, 100, 1920, 300], category="vfx",
                  confidence=0.72, depth_rank=7, label="飘落枫叶特效"),
    ]


def write_mock_detections(path: str) -> None:
    """把 mock 检测结果落成 JSON（detections 文件格式，供 CLI / 调试）。"""
    dets = make_mock_detections()
    payload = [
        {
            "bbox": [float(v) for v in d.bbox],
            "category": d.category,
            "confidence": d.confidence,
            "depth_rank": d.depth_rank,
            "depth_m": d.depth_m,
            "label": d.label,
            "mask_id": d.mask_id,
            "node_id": d.node_id,
        }
        for d in dets
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump({"detections": payload}, handle, ensure_ascii=False, indent=2)


def load_detections(path: str) -> List[Detection]:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    out = []
    for item in data.get("detections", []):
        out.append(Detection(
            bbox=item["bbox"],
            category=item["category"],
            confidence=float(item["confidence"]),
            depth_rank=item.get("depth_rank"),
            depth_m=item.get("depth_m"),
            label=item.get("label"),
            mask_id=item.get("mask_id"),
            node_id=item.get("node_id"),
        ))
    return out


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L1 分解层：检测结果 -> scene-ir.json")
    parser.add_argument("--detections", help="detections JSON 路径；缺省用 mock 数据")
    parser.add_argument("--out", default=str(ROOT / "build" / "scene-ir.json"),
                        help="输出 scene-ir.json 路径")
    parser.add_argument("--image-sha256", default="",
                        help="源截图 sha256（64 hex）；缺省按 detections 内容派生")
    parser.add_argument("--camera-json", default="",
                        help="可选相机参数 JSON（含 fov/pitch_deg/position/target）")
    parser.add_argument("--check", action="store_true",
                        help="装配后立即跑 verify_scene_ir 校验")
    args = parser.parse_args(argv)

    dets = load_detections(args.detections) if args.detections else make_mock_detections()

    camera = None
    if args.camera_json:
        with open(args.camera_json, "r", encoding="utf-8") as handle:
            camera = json.load(handle)

    sha = args.image_sha256
    if not sha:
        raw = json.dumps([d.__dict__ for d in dets], sort_keys=True)
        sha = hashlib.sha256(raw.encode("utf-8")).hexdigest()

    ir = decompose_to_ir(dets, sha, camera=camera)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(ir, handle, ensure_ascii=False, indent=2)
    print("wrote scene-ir -> %s" % args.out)

    if args.check:
        contract = verify_scene_ir.load_json(verify_scene_ir.CONTRACT_PATH)
        schema = verify_scene_ir.load_json(verify_scene_ir.SCHEMA_PATH)
        asset_ids, _ = verify_scene_ir.external_asset_ids()
        errors, warnings = verify_scene_ir.validate_ir(ir, contract, schema, asset_ids)
        print("errors: %d, warnings: %d" % (len(errors), len(warnings)))
        for e in errors:
            print("  [E] " + e)
        for w in warnings:
            print("  [W] " + w)
        return 1 if errors else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
