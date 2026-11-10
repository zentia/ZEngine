"""L1 视觉理解 provider：把截图喂给 VLM 模型，产出 `Detection` 列表。

设计
----
- 抽象 `VisionProvider.detect(image_path) -> list[Detection]`，与
  `decompose.py` 的确定性装配解耦：模型只负责「看到什么」（类别 + bbox +
  置信度 + 深度），确定性装配负责「归类 / 阈值 / 反投影」。
- `HunyuanVisionProvider` 是混元 vision 的实现，走 OpenAI 兼容端点
  `https://api.hunyuan.cloud.tencent.com/v1/chat/completions`（Bearer token，
  无需 TC3 签名）。
- API Key 只从环境变量 `HUNYUAN_API_KEY` 读，**绝不硬编码、绝不落盘**。
- 模型返回 JSON 解析失败 -> 抛 `VisionParseError`（不静默产出假数据）。

用法
----
    export HUNYUAN_API_KEY=...
    python scene-ir/vision_provider.py --image assets/worlds/jcc-maple-autumn.png \
        --out build/detections.json

结构化 prompt 约束
------------------
让模型输出一个 JSON 数组，每个元素：
    {"bbox": [x0,y0,x1,y1], "category": "<标签>", "confidence": 0.0-1.0,
     "depth_m": <可选深度>, "label": "<人读名>"}
category 用 `decompose.CATEGORY_TO_KIND` 里的白名单标签（ground/slab/pillar/
lantern/prop/character/vfx/...），未命中的标签由确定性层降级 unknown。
"""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import decompose  # noqa: E402
from decompose import Detection  # noqa: E402

HUNYUAN_ENDPOINT = "https://api.hunyuan.cloud.tencent.com/v1/chat/completions"
HUNYUAN_VISION_MODEL = "hunyuan-vision"

# 白名单类别标签（与 decompose.CATEGORY_TO_KIND 的 key 对齐），写进 prompt
# 让模型尽量用这些标签，未命中也由确定性层兜底降级 unknown。
_CATEGORY_HINT = (
    "ground/floor/board, structure/pillar/wall/frame, prop/decoration/lantern/"
    "orb/furniture, character/hero/monster/pet, vfx/particle/effect"
)


class VisionParseError(RuntimeError):
    """模型返回内容无法解析成 Detection 列表。"""


class VisionProvider:
    """视觉理解抽象接口。"""

    def detect(self, image_path: str) -> List[Detection]:
        raise NotImplementedError


def _load_image_base64(image_path: str) -> str:
    """读图 -> data URL（base64），按扩展名定 MIME。"""
    mime = mimetypes.guess_type(image_path)[0] or "image/png"
    if mime not in ("image/jpeg", "image/png", "image/webp", "image/bmp"):
        mime = "image/png"
    with open(image_path, "rb") as handle:
        data = handle.read()
    return "data:%s;base64,%s" % (mime, base64.b64encode(data).decode("ascii"))


def build_detection_prompt(image_width: int, image_height: int) -> str:
    """构造让模型输出结构化检测结果的 prompt（含坐标/类别约束）。"""
    return (
        "你是游戏场景分解器。请识别这张截图中所有独立的物体层（地面 / 结构 / "
        "道具 / 人物 / 特效），并按从远到近的层序输出一个 JSON 数组。\n"
        "\n"
        "图片尺寸：宽 %d 像素，高 %d 像素（bbox 用像素坐标，左上原点）。\n"
        "\n"
        "每个元素必须是如下 JSON 对象（不要输出任何其它文字，只输出 JSON 数组）：\n"
        "{\n"
        '  "bbox": [x0, y0, x1, y1],          // 像素坐标，0<=x0<x1<=宽，0<=y0<y1<=高\n'
        '  "category": "prop",               // 只用这些标签：%s\n'
        '  "confidence": 0.9,                // 0.0-1.0，低于 0.5 的模糊物体也照实输出\n'
        '  "depth_m": 8.0,                   // 该物体离相机的估算距离（米），远->近降序\n'
        '  "label": "朱红鼓车"                // 简短人读名，中文\n'
        "}\n"
        "\n"
        "规则：\n"
        "1. 地面 / 台面这种大块背景也作为一层，category 用 ground 或 board。\n"
        "2. 无法归类的物体 category 用 \"unknown\"。\n"
        "3. 只输出 JSON 数组本身，前后不要有任何解释文字、不要 markdown 代码块。\n"
    ) % (image_width, image_height, _CATEGORY_HINT)


def _extract_json_array(text: str) -> List[dict]:
    """从模型输出里稳健地抽 JSON 数组（容忍前后杂质 / markdown 代码块）。"""
    # 去掉 markdown 代码块围栏
    text = re.sub(r"```(?:json)?\s*", "", text)
    # 找第一个 '[' 和最后一个 ']'
    start = text.find("[")
    end = text.rfind("]")
    if start == -1 or end == -1 or end <= start:
        raise VisionParseError("模型输出里没有 JSON 数组: %r" % text[:200])
    try:
        parsed = json.loads(text[start:end + 1])
    except json.JSONDecodeError as exc:
        raise VisionParseError("JSON 解析失败: %s" % exc) from exc
    if not isinstance(parsed, list):
        raise VisionParseError("模型输出不是 JSON 数组: %r" % str(parsed)[:200])
    return parsed


def _dict_to_detection(item: Any) -> Detection:
    if not isinstance(item, dict):
        raise VisionParseError("检测项不是对象: %r" % str(item)[:100])
    bbox = item.get("bbox")
    if not isinstance(bbox, list) or len(bbox) != 4:
        raise VisionParseError("bbox 必须是 4 元素数组: %r" % str(item)[:100])
    category = str(item.get("category", "unknown")).strip()
    confidence = item.get("confidence")
    if confidence is None:
        confidence = 0.5
    confidence = float(confidence)
    depth = item.get("depth_m")
    depth = float(depth) if depth is not None else None
    return Detection(
        bbox=[float(v) for v in bbox],
        category=category,
        confidence=confidence,
        depth_m=depth,
        label=item.get("label"),
    )


class HunyuanVisionProvider(VisionProvider):
    """混元 vision 的 OpenAI 兼容实现。"""

    def __init__(self, api_key: Optional[str] = None,
                 endpoint: str = HUNYUAN_ENDPOINT,
                 model: str = HUNYUAN_VISION_MODEL,
                 timeout: int = 120) -> None:
        self.api_key = api_key or os.environ.get("HUNYUAN_API_KEY", "")
        self.endpoint = endpoint
        self.model = model
        self.timeout = timeout
        if not self.api_key:
            raise RuntimeError(
                "缺少 HUNYUAN_API_KEY：请在环境变量里设置腾讯云混元 API Key")

    def detect(self, image_path: str) -> List[Detection]:
        data_url = _load_image_base64(image_path)
        # 图片尺寸：从 data URL 解码 PNG/JPEG 头太麻烦，这里用 prompt 让模型
        # 自述，或用一个近似（混元 vision 能自己读图尺寸）。为稳妥，这里先
        # 用占位宽高，模型通常能按实际图片坐标返回。
        width, height = self._probe_image_size(image_path)
        prompt = build_detection_prompt(width, height)

        body = {
            "model": self.model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": prompt},
                        {"type": "image_url", "image_url": {"url": data_url}},
                    ],
                }
            ],
        }
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            self.endpoint,
            data=payload,
            headers={
                "Content-Type": "application/json",
                "Authorization": "Bearer " + self.api_key,
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                result = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            raise VisionParseError(
                "混元 API HTTP %d: %s" % (exc.code, detail[:500])) from exc

        # OpenAI 兼容返回结构：choices[0].message.content
        try:
            content = result["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise VisionParseError(
                "混元返回结构异常: %r" % str(result)[:500]) from exc

        items = _extract_json_array(content)
        return [_dict_to_detection(it) for it in items]

    @staticmethod
    def _probe_image_size(image_path: str) -> tuple[int, int]:
        """读 PNG/JPEG 尺寸头（无第三方库）。失败回退 1920x1080 占位。"""
        try:
            with open(image_path, "rb") as handle:
                head = handle.read(32)
            if head[:8] == b"\x89PNG\r\n\x1a\n":
                w = int.from_bytes(head[16:20], "big")
                h = int.from_bytes(head[20:24], "big")
                return w, h
            if head[:2] == b"\xff\xd8":  # JPEG，扫 SOF
                with open(image_path, "rb") as handle:
                    data = handle.read()
                return HunyuanVisionProvider._jpeg_size(data)
        except Exception:
            pass
        return 1920, 1080

    @staticmethod
    def _jpeg_size(data: bytes) -> tuple[int, int]:
        i = 2
        n = len(data)
        while i < n:
            if data[i] != 0xFF:
                i += 1
                continue
            marker = data[i + 1]
            if marker in (0xC0, 0xC1, 0xC2, 0xC3):
                h = int.from_bytes(data[i + 5:i + 7], "big")
                w = int.from_bytes(data[i + 7:i + 9], "big")
                return w, h
            seg_len = int.from_bytes(data[i + 2:i + 4], "big")
            i += 2 + seg_len
        return 1920, 1080


class MockVisionProvider(VisionProvider):
    """兜底 / 测试用：不调模型，直接返回 mock 检测结果。"""

    def detect(self, image_path: str) -> List[Detection]:
        return decompose.make_mock_detections()


def load_detections(path: str) -> List[Detection]:
    """读 detections JSON（与 decompose.load_detections 同口径，供调试）。"""
    return decompose.load_detections(path)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L1 视觉理解：截图 -> Detection 列表")
    parser.add_argument("--image", required=True, help="截图路径")
    parser.add_argument("--out", required=True, help="输出 detections JSON 路径")
    parser.add_argument("--mock", action="store_true", help="用 mock provider，不调模型")
    parser.add_argument("--model", default=HUNYUAN_VISION_MODEL)
    args = parser.parse_args(argv)

    provider: VisionProvider = MockVisionProvider() if args.mock else HunyuanVisionProvider(model=args.model)
    detections = provider.detect(args.image)

    payload = [
        {
            "bbox": [float(v) for v in d.bbox],
            "category": d.category,
            "confidence": d.confidence,
            "depth_m": d.depth_m,
            "label": d.label,
            "mask_id": d.mask_id,
        }
        for d in detections
    ]
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump({"detections": payload}, handle, ensure_ascii=False, indent=2)
    print("wrote %d detections -> %s" % (len(detections), args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
