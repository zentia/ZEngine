"""L4 对拍（compare.py）门禁。

只测确定性对拍算法（bbox IoU / 分层归并 / pixel_delta / 达标判定 / 归因），
不涉及真实 Unity 渲染（渲染帧本身是外部依赖）。
运行：python verify/test_compare.py
"""

from __future__ import annotations

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scene-ir"))

import compare  # noqa: E402
import decompose  # noqa: E402
from decompose import Detection  # noqa: E402


def det(bbox, category, confidence=0.9, **kw):
    return Detection(bbox=bbox, category=category, confidence=confidence, **kw)


class BboxIouTests(unittest.TestCase):
    def test_identical_boxes(self) -> None:
        self.assertAlmostEqual(compare.bbox_iou([0, 0, 10, 10], [0, 0, 10, 10]), 1.0)

    def test_disjoint_boxes(self) -> None:
        self.assertEqual(compare.bbox_iou([0, 0, 10, 10], [20, 20, 30, 30]), 0.0)

    def test_half_overlap(self) -> None:
        # 两个 10x10，横向重叠一半 -> IoU = 50/(100+100-50) = 1/3
        iou = compare.bbox_iou([0, 0, 10, 10], [5, 0, 15, 10])
        self.assertAlmostEqual(iou, 1.0 / 3.0, places=6)

    def test_contained_box(self) -> None:
        # 小框完全在大框内 -> IoU = 小/大
        iou = compare.bbox_iou([0, 0, 10, 10], [2, 2, 4, 4])
        self.assertAlmostEqual(iou, 4.0 / 100.0, places=6)


class GroupByKindTests(unittest.TestCase):
    def test_groups_by_kind(self) -> None:
        dets = [
            det([0, 0, 10, 10], "ground"),
            det([0, 0, 10, 10], "board"),  # board -> ground
            det([0, 0, 10, 10], "lantern"),  # lantern -> prop
            det([0, 0, 10, 10], "character"),  # 不参与
        ]
        groups = compare.group_by_kind(dets)
        self.assertIn("ground", groups)
        self.assertIn("prop", groups)
        self.assertEqual(len(groups["ground"]), 2)
        self.assertEqual(len(groups["prop"]), 1)
        self.assertNotIn("character", groups)


class CompareLayerTests(unittest.TestCase):
    def test_perfect_match(self) -> None:
        b = [det([0, 0, 10, 10], "prop", label="a")]
        r = [det([0, 0, 10, 10], "prop", label="b")]
        layer = compare.compare_layer(b, r)
        self.assertAlmostEqual(layer["mean_iou"], 1.0)
        self.assertEqual(layer["missed"], 0)
        self.assertEqual(layer["extra"], 0)

    def test_missed_when_no_match(self) -> None:
        b = [det([0, 0, 10, 10], "prop")]
        r = []  # 渲染帧没有 -> 漏放
        layer = compare.compare_layer(b, r)
        self.assertEqual(layer["missed"], 1)
        self.assertEqual(layer["mean_iou"], 0.0)

    def test_extra_when_render_has_more(self) -> None:
        b = [det([0, 0, 10, 10], "prop")]
        r = [det([0, 0, 10, 10], "prop"), det([50, 50, 60, 60], "prop")]
        layer = compare.compare_layer(b, r)
        self.assertEqual(layer["extra"], 1)


class CompareDetectionsTests(unittest.TestCase):
    def test_perfect_report_passes(self) -> None:
        b = [det([0, 0, 100, 100], "board"), det([10, 10, 50, 50], "prop")]
        r = [det([0, 0, 100, 100], "board"), det([10, 10, 50, 50], "prop")]
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        self.assertTrue(report["overall"]["pass"])
        self.assertAlmostEqual(report["overall"]["mean_iou"], 1.0)

    def test_mismatch_fails_and_attributes(self) -> None:
        b = [det([0, 0, 100, 100], "board")]
        r = []  # 全部漏放
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        self.assertFalse(report["overall"]["pass"])
        self.assertGreater(len(report["attribution"]), 0)
        # 归因应指向 ground（board -> ground）
        kinds = [a["kind"] for a in report["attribution"]]
        self.assertIn("ground", kinds)

    def test_pixel_delta_computed(self) -> None:
        # 基准 100x100=10000，渲染 50x50=2500，图 200x200=40000
        # delta = |10000-2500|/40000 = 0.1875
        b = [det([0, 0, 100, 100], "board")]
        r = [det([0, 0, 50, 50], "board")]
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        layer = report["layers"]["ground"]
        self.assertAlmostEqual(layer["pixel_delta"], 0.1875, places=4)

    def test_no_comparable_kinds(self) -> None:
        # 只有 character（不参与）-> 空报告，overall pass 为 True（无层可比）
        b = [det([0, 0, 10, 10], "character")]
        r = [det([0, 0, 10, 10], "character")]
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        self.assertEqual(len(report["layers"]), 0)


class NodeLevelAttributionTests(unittest.TestCase):
    """L4 归因下沉到 node 级：配对优先 node_id，attribution 带 nodes[]。"""

    def test_node_id_exact_pairing(self) -> None:
        # 两侧同 node_id 直接配对，即使 bbox 略偏移也能对上（IoU 按 bbox 算）
        b = [det([0, 0, 10, 10], "prop", node_id="lantern_w", label="左灯")]
        r = [det([2, 2, 12, 12], "prop", node_id="lantern_w", label="左灯")]
        layer = compare.compare_layer(b, r)
        self.assertEqual(layer["missed"], 0)
        self.assertEqual(layer["extra"], 0)
        self.assertEqual(layer["nodes"][0]["matched_by"], "node_id")
        self.assertEqual(layer["nodes"][0]["node_id"], "lantern_w")

    def test_missing_node_attributed_with_node_id(self) -> None:
        # 基准有、渲染无 -> attribution.nodes 带该节点的 node_id + status=missed
        b = [det([0, 0, 10, 10], "prop", node_id="lantern_c", label="中灯")]
        r = []
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        nodes = report["attribution"][0]["nodes"]
        self.assertEqual(len(nodes), 1)
        self.assertEqual(nodes[0]["node_id"], "lantern_c")
        self.assertEqual(nodes[0]["status"], "missed")

    def test_low_iou_pair_attributed_as_offset(self) -> None:
        # 同 node_id 配对但 IoU 低 -> status=paired + 位置/尺度偏移 reason
        b = [det([0, 0, 10, 10], "prop", node_id="lantern_e", label="右灯")]
        r = [det([50, 50, 60, 60], "prop", node_id="lantern_e", label="右灯")]
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        nodes = report["attribution"][0]["nodes"]
        self.assertEqual(nodes[0]["status"], "paired")
        self.assertIn("偏移", nodes[0]["reason"])
        self.assertEqual(nodes[0]["matched_node_id"], "lantern_e")

    def test_extra_node_attributed_with_node_id(self) -> None:
        # 渲染多放 -> status=extra
        b = [det([0, 0, 10, 10], "prop", node_id="lantern_w", label="左灯")]
        r = [
            det([0, 0, 10, 10], "prop", node_id="lantern_w", label="左灯"),
            det([50, 50, 60, 60], "prop", node_id="lantern_extra", label="多放灯"),
        ]
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        statuses = [n["status"] for n in report["attribution"][0]["nodes"]]
        self.assertIn("extra", statuses)
        extra = next(n for n in report["attribution"][0]["nodes"]
                     if n["status"] == "extra")
        self.assertEqual(extra["node_id"], "lantern_extra")

    def test_fallback_to_bbox_when_no_node_id(self) -> None:
        # 无 node_id 时退回 bbox 贪心配对，matched_by=bbox，仍能 node 级输出
        b = [det([0, 0, 10, 10], "prop", label="a")]
        r = [det([0, 0, 10, 10], "prop", label="b")]
        layer = compare.compare_layer(b, r)
        self.assertEqual(layer["missed"], 0)
        self.assertEqual(layer["nodes"][0]["matched_by"], "bbox")

    def test_passing_node_not_in_attribution(self) -> None:
        # 达标配对（IoU>=0.9 且无漏/多放）不出现在 attribution.nodes 里
        b = [det([0, 0, 100, 100], "prop", node_id="a", label="x")]
        r = [det([0, 0, 100, 100], "prop", node_id="a", label="x")]
        report = compare.compare_detections(
            b, r, (200, 200), {"iou": 0.9, "pixel_delta": 0.05})
        self.assertEqual(report["attribution"], [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
