"""L4 对拍：基准帧 vs 渲染帧的分层 IoU / pixel_delta / 差异归因（确定性）。

职责边界（与 contract.ai_boundary 一致）
----------------------------------------
- L4 对拍回答「还原得像不像」：把基准帧（ground truth，金铲铲原图）与渲染帧
  （我们生成的场景在锁机位渲染出的图）都做分层检测（复用 L1 的语义检测），
  然后对**同一语义层**（ground/structure/prop/...）比对 bbox 的 IoU 与整图
  分层 pixel_delta。
- AI 负责「语义差异归因」（哪个层像、哪个层不像、为什么）；确定性代码负责
  「锁机位渲染、分层 IoU diff、门禁」——本模块就是确定性门禁部分。
- **诚实边界**：只有锁机位对拍可自动化（自由环视无唯一 ground truth，见需求
  文档第 9 节）。渲染帧本身需要 Unity 锁机位渲染（外部依赖），本模块只做
  「两张图各自分层检测结果 -> IoU / pixel_delta / 归因」的确定性比对。

对拍算法
--------
1. 分层归并：按 category 把两边的检测结果分组（同一语义层）。
2. 分层 IoU：同层内，对基准帧每个 bbox 找渲染帧里 IoU 最大的 bbox，取均值
   作为该层的 IoU；渲染帧多出的 bbox 记为「漏放/多放」。
3. pixel_delta：整图归一化 bbox 面积差（按层累加 |面积差| / 图面积）。
4. 归因：每层输出 IoU / pixel_delta / 是否达标（对照 acceptance.tolerance），
   未达标层指向对应节点 id。归因已下沉到 node 级：`attribution[].nodes[]`
   携带每个漏放 / 多放 / 低 IoU 节点的 node_id + label + reason，供 L5 补丁
   「建议改哪个节点、改什么」直接消费（配对优先按 node_id 精确匹配，无
   node_id 时退回 bbox IoU 贪心匹配兜底）。

用法
----
    python scene-ir/compare.py --baseline build/baseline_detections.json \
        --render build/render_detections.json \
        --image-size 1920x864 [--tolerance '{"iou":0.9,"pixel_delta":0.05}'] \
        --out build/compare_report.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import decompose  # noqa: E402
from decompose import Detection  # noqa: E402

# 参与对拍的分层（与 scene-ir 的 ASSIGNABLE_KINDS 对应，只有场景类 kind）。
# character / vfx 在 L1 已降级 unknown，不参与 L4 几何对拍。
COMPARABLE_KINDS = frozenset({"ground", "structure", "prop"})


def bbox_iou(a: Sequence[float], b: Sequence[float]) -> float:
    """两个 bbox [x0,y0,x1,y1] 的 IoU（交集/并集）；无交集返回 0。"""
    ax0, ay0, ax1, ay1 = (float(v) for v in a)
    bx0, by0, bx1, by1 = (float(v) for v in b)
    ix0 = max(ax0, bx0)
    iy0 = max(ay0, by0)
    ix1 = min(ax1, bx1)
    iy1 = min(ay1, by1)
    if ix1 <= ix0 or iy1 <= iy0:
        return 0.0
    inter = (ix1 - ix0) * (iy1 - iy0)
    area_a = (ax1 - ax0) * (ay1 - ay0)
    area_b = (bx1 - bx0) * (by1 - by0)
    union = area_a + area_b - inter
    if union <= 0:
        return 0.0
    return inter / union


def bbox_area(a: Sequence[float]) -> float:
    x0, y0, x1, y1 = (float(v) for v in a)
    return max(0.0, (x1 - x0) * (y1 - y0))


def group_by_kind(detections: Sequence[Detection]) -> Dict[str, List[Detection]]:
    """按 category -> kind 归并检测结果，只保留可对拍的三类 kind。"""
    groups: Dict[str, List[Detection]] = {}
    for det in detections:
        kind = decompose.map_kind(det.category)
        if kind not in COMPARABLE_KINDS:
            continue
        groups.setdefault(kind, []).append(det)
    return groups


def compare_layer(baseline: List[Detection], render: List[Detection]) -> Dict[str, Any]:
    """同一层内，基准帧 vs 渲染帧的 IoU 与数量差异，并下沉到 node 级明细。

    配对优先级：
    1. 按 node_id 精确配对：两侧 node_id 相同（非空）直接配对，算 IoU。
    2. 剩余未配对的用 bbox 贪心 IoU 兜底（兼容无 node_id 的旧检测数据）。

    对基准帧每个 bbox，找渲染帧里 IoU 最大的 bbox 配对；无配对记为 miss。
    渲染帧未被配对的 bbox 记为 extra。返回 node 级明细供 L5 补丁定位。
    """
    render_boxes = [d.bbox for d in render]
    paired = [False] * len(render_boxes)

    # 预索引渲染帧的 node_id -> 下标（用于精确配对）
    render_by_id: Dict[str, int] = {}
    for j, d in enumerate(render):
        if d.node_id:
            render_by_id.setdefault(d.node_id, j)

    ious: List[float] = []
    misses = 0
    node_details: List[Dict[str, Any]] = []
    for bdet in baseline:
        best_iou = 0.0
        best_idx = -1
        matched_by = "bbox"

        # 1) node_id 精确配对：两侧同 node_id 即视为同一节点，**不依赖 IoU>0**。
        #    同 node_id 但 bbox 差很远仍是「位置/尺度偏移」，不是「漏放」——
        #    补丁动作是调 anchor/scale，不是重新生成。IoU 只量化偏移程度。
        if bdet.node_id and bdet.node_id in render_by_id:
            j = render_by_id[bdet.node_id]
            if not paired[j]:
                best_idx = j
                best_iou = bbox_iou(bdet.bbox, render[j].bbox)
                matched_by = "node_id"

        # 2) bbox 贪心兜底（无 node_id 或 node_id 未命中）
        if best_idx < 0:
            for j, rbox in enumerate(render_boxes):
                if paired[j]:
                    continue
                iou = bbox_iou(bdet.bbox, rbox)
                if iou > best_iou:
                    best_iou = iou
                    best_idx = j

        if best_idx >= 0 and (matched_by == "node_id" or best_iou > 0.0):
            paired[best_idx] = True
            ious.append(best_iou)
            node_details.append({
                "node_id": bdet.node_id,
                "label": bdet.label,
                "matched_node_id": render[best_idx].node_id,
                "matched_label": render[best_idx].label,
                "iou": round(best_iou, 4),
                "matched_by": matched_by,
                "status": "paired",
            })
        else:
            misses += 1
            node_details.append({
                "node_id": bdet.node_id,
                "label": bdet.label,
                "iou": 0.0,
                "matched_by": None,
                "status": "missed",
            })

    # 渲染帧未被配对的 -> extra
    for j, d in enumerate(render):
        if not paired[j]:
            node_details.append({
                "node_id": d.node_id,
                "label": d.label,
                "iou": 0.0,
                "matched_by": None,
                "status": "extra",
            })

    extras = sum(1 for p in paired if not p)

    mean_iou = sum(ious) / len(ious) if ious else 0.0
    return {
        "mean_iou": round(mean_iou, 4),
        "baseline_count": len(baseline),
        "render_count": len(render),
        "missed": misses,       # 基准帧有、渲染帧没有（漏放）
        "extra": extras,        # 渲染帧有、基准帧没有（多放）
        "pair_iou": [round(v, 4) for v in ious],
        "nodes": node_details,  # node 级明细（供 L5 补丁定位）
    }


def compare_detections(
    baseline: Sequence[Detection],
    render: Sequence[Detection],
    image_size: Tuple[int, int],
    tolerance: Dict[str, float],
) -> Dict[str, Any]:
    """分层对拍主入口：产出分层 IoU / pixel_delta / 达标判定 / 归因。"""
    width, height = image_size
    image_area = float(width) * float(height)
    if image_area <= 0:
        raise ValueError("image_size 必须为正")

    base_groups = group_by_kind(baseline)
    render_groups = group_by_kind(render)

    all_kinds = sorted(set(base_groups) | set(render_groups))
    iou_threshold = float(tolerance.get("iou", 0.9))
    pixel_threshold = float(tolerance.get("pixel_delta", 0.05))

    layers: Dict[str, Any] = {}
    total_pixel_delta = 0.0
    for kind in all_kinds:
        b = base_groups.get(kind, [])
        r = render_groups.get(kind, [])
        layer = compare_layer(b, r)

        # 该层的 pixel_delta：基准帧与渲染帧的 bbox 总面积差（归一化到图面积）
        base_area = sum(bbox_area(d.bbox) for d in b)
        render_area = sum(bbox_area(d.bbox) for d in r)
        layer_delta = abs(base_area - render_area) / image_area
        total_pixel_delta += layer_delta
        layer["pixel_delta"] = round(layer_delta, 6)

        # 达标判定：mean_iou >= threshold 且 pixel_delta <= threshold，且
        # 无漏放/多放（miss/extra 必须为 0）。漏放/多放是「节点数量/身份」错误，
        # 不能靠 IoU/pixel_delta 的均值掩盖（否则 extra 场景下整层仍误判 pass）。
        layer["iou_pass"] = layer["mean_iou"] >= iou_threshold
        layer["pixel_pass"] = layer_delta <= pixel_threshold
        layer["count_pass"] = layer["missed"] == 0 and layer["extra"] == 0
        layer["pass"] = layer["iou_pass"] and layer["pixel_pass"] and layer["count_pass"]
        layers[kind] = layer

    overall_iou = sum(l["mean_iou"] for l in layers.values()) / len(layers) if layers else 0.0
    report: Dict[str, Any] = {
        "schema": "zengine.compare/v1",
        "image_size": [width, height],
        "tolerance": tolerance,
        "layers": layers,
        "overall": {
            "mean_iou": round(overall_iou, 4),
            "pixel_delta": round(total_pixel_delta, 6),
            "iou_pass": overall_iou >= iou_threshold,
            "pixel_pass": total_pixel_delta <= pixel_threshold,
            "pass": overall_iou >= iou_threshold and total_pixel_delta <= pixel_threshold,
        },
        # 归因：未达标层的归因线索（供 AI 语义归因 + L5 补丁定位）
        "attribution": [
            {
                "kind": kind,
                "mean_iou": layers[kind]["mean_iou"],
                "missed": layers[kind]["missed"],
                "extra": layers[kind]["extra"],
                "note": _attribution_note(kind, layers[kind]),
                # node 级定位：该层未达标 / 未配对的节点，带 node_id + label，
                # 供 L5 补丁「建议改哪个节点、改什么」直接消费。
                "nodes": _attribution_nodes(kind, layers[kind]),
            }
            for kind in all_kinds
            if not layers[kind]["pass"]
        ],
    }
    return report


def _attribution_note(kind: str, layer: Dict[str, Any]) -> str:
    parts = []
    if layer["missed"] > 0:
        parts.append("漏放 %d 个（基准帧有、渲染帧无）" % layer["missed"])
    if layer["extra"] > 0:
        parts.append("多放 %d 个（渲染帧有、基准帧无）" % layer["extra"])
    if layer["mean_iou"] < 0.9 and layer["missed"] == 0 and layer["extra"] == 0:
        parts.append("位置/尺度偏移（配对但 IoU 低）")
    if not parts:
        parts.append("面积偏差超阈值")
    return "；".join(parts)


def _attribution_nodes(kind: str, layer: Dict[str, Any]) -> List[Dict[str, Any]]:
    """把该层的 node 级明细收敛成 L5 补丁可消费的定位清单。

    只保留「需要处理」的节点：missed / extra / 低 IoU 配对（IoU 低于 0.9
    即位置或尺度偏移）。每项带 node_id / label / status / iou / reason，
    node_id 为空时退回 label（人读）作定位线索。
    """
    out: List[Dict[str, Any]] = []
    for n in layer.get("nodes", []):
        status = n.get("status")
        iou = float(n.get("iou", 0.0))
        if status == "paired" and iou >= 0.9:
            continue  # 达标配对，无需处理
        item: Dict[str, Any] = {
            "kind": kind,
            "node_id": n.get("node_id"),
            "label": n.get("label"),
            "status": status,
            "iou": round(iou, 4),
        }
        if status == "missed":
            item["reason"] = "基准帧有、渲染帧无（漏放）"
        elif status == "extra":
            item["reason"] = "渲染帧有、基准帧无（多放）"
        else:
            item["reason"] = "位置/尺度偏移（配对但 IoU 低）"
            item["matched_node_id"] = n.get("matched_node_id")
        out.append(item)
    return out


def load_detections(path: str) -> List[Detection]:
    return decompose.load_detections(path)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L4 对拍：基准帧 vs 渲染帧分层 IoU")
    parser.add_argument("--baseline", required=True, help="基准帧 detections JSON")
    parser.add_argument("--render", required=True, help="渲染帧 detections JSON")
    parser.add_argument("--image-size", default="1920x864", help="图像尺寸 WxH")
    parser.add_argument("--tolerance", default='{"iou":0.9,"pixel_delta":0.05}',
                        help="验收容差 JSON")
    parser.add_argument("--out", required=True, help="输出对拍报告 JSON")
    args = parser.parse_args(argv)

    baseline = load_detections(args.baseline)
    render = load_detections(args.render)
    w, h = (int(v) for v in args.image_size.split("x"))
    tolerance = json.loads(args.tolerance)

    report = compare_detections(baseline, render, (w, h), tolerance)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
    print("wrote compare report -> %s" % args.out)

    ov = report["overall"]
    print("overall: mean_iou=%.4f pixel_delta=%.4f pass=%s"
          % (ov["mean_iou"], ov["pixel_delta"], ov["pass"]))
    for kind, layer in report["layers"].items():
        print("  [%s] iou=%.4f missed=%d extra=%d pass=%s"
              % (kind, layer["mean_iou"], layer["missed"], layer["extra"],
                 layer["pass"]))
    return 0 if ov["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
