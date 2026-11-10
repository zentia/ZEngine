"""L1 深度可信度评估：VLM 单目 depth_m 到底是「绝对米」还是「相对层序」。

评估方法（对同一张图跑多组受控提问，比对深度输出）：

  组1  absolute_m    直接问「离相机多少米」（现状 prompt 风格）
  组2  absolute_m_2  同组1 再跑一次，测同一物体绝对米数的漂移（一致性）
  组3  relative_rank 只问「从远到近排序 + 0~10 相对深度分」（不给米）
  组4  absolute_emph 强调「真实世界米制」，试探模型是否随措辞编数

输出：每组探测结果 JSON + 一个汇总分析（相对层序是否稳定 / 绝对米是否
自洽 / 绝对米跨轮漂移幅度）。

用法：
    python scene-ir/eval_depth.py --image assets/concept.jpg \
        --out build/depth_eval.json
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import vision_provider as vp  # noqa: E402


# ---------------------------------------------------------------------------
# 四种提问方式
# ---------------------------------------------------------------------------

_PROMPTS: Dict[str, str] = {
    # 组1 / 组2：绝对米（现状风格）
    "absolute_m": (
        "你是游戏场景分解器。请识别这张截图中所有独立的物体层，并按从远到近"
        "输出一个 JSON 数组。每个元素：{\"bbox\":[x0,y0,x1,y1],\"category\":\"<标签>\","
        "\"confidence\":0~1,\"depth_m\":<该物体离相机的估算距离，单位米>,\"label\":\"<人读名>\"}。"
        "bbox 用像素坐标。category 用 ground/board/structure/prop/character/vfx。"
        "depth_m 请给出你认为的真实米制距离。只输出 JSON 数组本身。"
    ),
    # 组4：强调真实米制
    "absolute_emph": (
        "你是游戏场景分解器。这张图是一个棋盘游戏场景的截图。请识别所有独立"
        "物体层，从远到近输出 JSON 数组。每个元素：{\"bbox\":[x0,y0,x1,y1],"
        "\"category\":\"<标签>\",\"confidence\":0~1,\"depth_m\":<距离>,\"label\":\"<人读名>\"}。"
        "特别注意：depth_m 必须是真实世界中的米制距离（例如一个棋盘台面通常"
        "在相机前方 2~6 米，背景山峦可能在 50~200 米）。请基于透视和常识认真"
        "估算，不要随意编一个数。只输出 JSON 数组本身。"
    ),
    # 组3：相对层序（不给米）
    "relative_rank": (
        "你是游戏场景分解器。请识别这张截图中所有独立的物体层，并按从远到近"
        "输出一个 JSON 数组。每个元素：{\"bbox\":[x0,y0,x1,y1],\"category\":\"<标签>\","
        "\"confidence\":0~1,\"depth_rank\":<0~10 的相对深度分，0=最远，10=最近>,\"label\":\"<人读名>\"}。"
        "depth_rank 只表达相对远近顺序，不是米。只输出 JSON 数组本身。"
    ),
}

# 用几组（组2 = 组1 复跑）
_RUNS: List[tuple] = [
    ("absolute_m", "absolute_m"),
    ("absolute_m_2", "absolute_m"),   # 复跑组1
    ("relative_rank", "relative_rank"),
    ("absolute_emph", "absolute_emph"),
]


def _call_vlm(image_path: str, prompt: str) -> str:
    """调 WorkBuddy VLM，返回模型文本输出。"""
    data_url = vp._load_image_base64(image_path)
    body = {
        "model": os.environ.get("WORKBUDDY_VLM_MODEL", vp.WORKBUDDY_VLM_MODEL),
        "stream": True,
        "messages": [
            {"role": "user", "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url", "image_url": {"url": data_url}},
            ]},
        ],
    }
    req = urllib.request.Request(
        os.environ.get("WORKBUDDY_VLM_CHAT_URL", vp.WORKBUDDY_VLM_ENDPOINT),
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + os.environ["WORKBUDDY_VLM_AUTH_TOKEN"],
            "X-WorkBuddy-MCP-Context": os.environ["WORKBUDDY_VLM_CONTEXT"],
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        raw = resp.read().decode("utf-8")
    return vp._parse_sse_stream(raw)


def _parse_items(text: str) -> List[dict]:
    """抽 JSON 数组（复用 vision_provider 的稳健抽取）。"""
    return vp._extract_json_array(text)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L1 深度可信度评估")
    parser.add_argument("--image", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    results: Dict[str, Any] = {"image": args.image, "runs": {}}
    for run_name, prompt_key in _RUNS:
        prompt = _PROMPTS[prompt_key]
        print("== 跑 %s（%s）==" % (run_name, prompt_key), flush=True)
        try:
            text = _call_vlm(args.image, prompt)
            items = _parse_items(text)
            results["runs"][run_name] = {
                "prompt_key": prompt_key,
                "raw": text,
                "items": items,
            }
            for it in items:
                print("   %-14s cat=%-8s depth_m=%s depth_rank=%s" % (
                    it.get("label", ""), it.get("category", ""),
                    it.get("depth_m"), it.get("depth_rank")))
        except Exception as exc:  # noqa: BLE001
            results["runs"][run_name] = {"error": str(exc)}
            print("   失败: %s" % exc)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(results, handle, ensure_ascii=False, indent=2)
    print("\nwrote -> %s" % args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
