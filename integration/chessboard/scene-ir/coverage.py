"""成分覆盖对拍（阶段一验收）：场景语义清单 vs 基准语义清单。

职责边界
--------
这是「概念图 → 真实场景」在无相机参数下的第一道还原度验收——**语义成分覆盖**。
回答三个问题：
  1. 覆盖（covered）：基准清单里的语义成分，场景里有没有对应物？
  2. 漏项（missing）：基准里有、场景里没有的成分（还原漏了）。
  3. 多项（extra）：场景里有、基准里没有的成分（多放了）。

不依赖相机参数、不做像素 IoU（那是 L4 的职责，需锁机位渲染）。这里只比对
「该有的东西在不在」，用语义成分的 name / kind / position 做匹配。

用法
----
    python scene-ir/coverage.py --baseline build/baseline_manifest.json \
        --scene build/scene_manifest.json \
        --out build/coverage_report.json

匹配规则
--------
按 name 精确匹配优先；未命中时退 kind + position 模糊匹配（同 kind 同方位
视为覆盖，容忍命名差异）。core=true 的成分漏项会拉高 critical_missing。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))


def _norm(s: str) -> str:
    return s.strip().lower()


def _match(
    comp: Dict[str, Any],
    scene_comps: List[Dict[str, Any]],
    used: List[bool],
    prefer_exact: bool,
) -> Optional[int]:
    """给基准成分找一个场景成分匹配。

    两阶段：先全局 name 精确匹配（prefer_exact=True 时），未命中再退
    kind + position 模糊匹配（容忍命名差异）。分两阶段避免贪心错位——
    否则同 kind+position 的多个成分会把前面的场景项抢走，导致后面的
    name 精确可匹配的成分反而 miss。
    """
    if prefer_exact:
        for i, sc in enumerate(scene_comps):
            if used[i]:
                continue
            if _norm(sc.get("name", "")) == _norm(comp.get("name", "")):
                return i
        return None
    # kind + position 模糊匹配
    for i, sc in enumerate(scene_comps):
        if used[i]:
            continue
        if (sc.get("kind") == comp.get("kind")
                and sc.get("position") == comp.get("position")):
            return i
    return None


def coverage(
    baseline: Dict[str, Any],
    scene: Dict[str, Any],
) -> Dict[str, Any]:
    base_comps = baseline.get("components", [])
    scene_comps = scene.get("components", [])

    used = [False] * len(scene_comps)
    matched: Dict[int, int] = {}  # 基准成分索引 -> 场景成分索引

    # 阶段 1：name 精确匹配（全局优先）
    for bi, comp in enumerate(base_comps):
        idx = _match(comp, scene_comps, used, prefer_exact=True)
        if idx is not None:
            used[idx] = True
            matched[bi] = idx

    # 阶段 2：kind + position 模糊匹配（剩余未匹配的基准成分）
    for bi, comp in enumerate(base_comps):
        if bi in matched:
            continue
        idx = _match(comp, scene_comps, used, prefer_exact=False)
        if idx is not None:
            used[idx] = True
            matched[bi] = idx

    covered: List[Dict[str, Any]] = []
    missing: List[Dict[str, Any]] = []
    for bi, comp in enumerate(base_comps):
        idx = matched.get(bi)
        if idx is None:
            missing.append(comp)
        else:
            covered.append({
                "id": comp.get("id"),
                "name": comp.get("name"),
                "matched_scene_id": scene_comps[idx].get("id"),
                "matched_scene_name": scene_comps[idx].get("name"),
                "matched_by": "exact" if _norm(scene_comps[idx].get("name", "")) == _norm(comp.get("name", "")) else "kind_position",
                "core": comp.get("core", False),
            })

    extra = [
        scene_comps[i] for i in range(len(scene_comps)) if not used[i]
    ]

    total = len(base_comps)
    covered_count = len(covered)
    missing_count = len(missing)
    extra_count = len(extra)
    critical_missing = [c for c in missing if c.get("core")]
    critical_count = len(critical_missing)

    # 覆盖率的三种口径
    coverage_rate = (covered_count / total) if total else 1.0
    core_rate = (1.0 - (critical_count / max(1, sum(1 for c in base_comps if c.get("core")))))

    report = {
        "schema": "zengine.coverage-report/v1",
        "baseline_name": baseline.get("name"),
        "baseline_sha256": baseline.get("source_image_sha256"),
        "scene_name": scene.get("name"),
        "total_components": total,
        "covered": covered_count,
        "missing": missing_count,
        "extra": extra_count,
        "critical_missing": critical_count,
        "coverage_rate": round(coverage_rate, 4),
        "core_coverage_rate": round(core_rate, 4),
        "covered_items": covered,
        "missing_items": missing,
        "extra_items": extra,
        "pass": coverage_rate >= 1.0 and critical_count == 0,
        "pass_note": "全部成分覆盖 + 无核心漏项" if coverage_rate >= 1.0 and critical_count == 0
                     else "存在漏项（missing > 0 或核心成分漏放）",
    }
    return report


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="成分覆盖对拍：场景语义清单 vs 基准语义清单")
    parser.add_argument("--baseline", required=True, help="基准语义清单 JSON")
    parser.add_argument("--scene", required=True, help="场景语义清单 JSON")
    parser.add_argument("--out", required=True, help="输出覆盖报告 JSON")
    args = parser.parse_args(argv)

    with open(args.baseline, "r", encoding="utf-8") as handle:
        baseline = json.load(handle)
    with open(args.scene, "r", encoding="utf-8") as handle:
        scene = json.load(handle)

    report = coverage(baseline, scene)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
    print("wrote coverage report -> %s" % args.out)
    print("coverage_rate=%.4f core_rate=%.4f missing=%d extra=%d critical=%d pass=%s"
          % (report["coverage_rate"], report["core_coverage_rate"],
             report["missing"], report["extra"], report["critical_missing"],
             report["pass"]))
    for m in report["missing_items"]:
        tag = "[核心]" if m.get("core") else ""
        print("  [MISS] %s %s" % (m.get("name"), tag))
    for e in report["extra_items"]:
        print("  [EXTRA] %s" % e.get("name"))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
