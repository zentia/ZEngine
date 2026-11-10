"""L1 视觉理解 provider：把截图喂给 VLM 模型，产出 `Detection` 列表。

设计
----
- 抽象 `VisionProvider.detect(image_path) -> list[Detection]`，与
  `decompose.py` 的确定性装配解耦：模型只负责「看到什么」（类别 + bbox +
  置信度 + 深度），确定性装配负责「归类 / 阈值 / 反投影」。
- **默认走 WorkBuddy 内部 VLM sidecar（IOA 授权，无需 API Key）**：
  `WorkBuddyVLMProvider` 从环境变量读鉴权材料，调用本地流式 chat 端点
  `WORKBUDDY_VLM_CHAT_URL`（OpenAI 兼容的 SSE 流，`stream: true`）。
  - `WORKBUDDY_VLM_AUTH_TOKEN` -> `Authorization: Bearer <token>`
  - `WORKBUDDY_VLM_CONTEXT`   -> `X-WorkBuddy-MCP-Context: <context>`
  - `WORKBUDDY_VLM_MODEL`     -> 模型名（缺省 `kinfra-office-agent`）
  这套变量由 WorkBuddy 在 IOA 环境下自动注入，用户无需提供任何 key。
- `HunyuanVisionProvider` 保留为**备用通道**（显式传 api_key 才用），走
  腾讯云混元公网 OpenAI 端点，适用于脱离 WorkBuddy 的独立运行场景。
- API Key 只从环境变量读，**绝不硬编码、绝不落盘**。
- 模型返回 JSON 解析失败 -> 抛 `VisionParseError`（不静默产出假数据）。

用法
----
    # WorkBuddy IOA 环境（无需 key）
    python scene-ir/vision_provider.py --image assets/worlds/jcc-maple-autumn.png \
        --out build/detections.json

    # 显式走混元公网（需 HUNYUAN_API_KEY）
    HUNYUAN_API_KEY=... python scene-ir/vision_provider.py \
        --provider hunyuan --image ... --out ...

结构化 prompt 约束
------------------
让模型输出一个 JSON 数组，每个元素：
    {"bbox": [x0,y0,x1,y1], "category": "<标签>", "confidence": 0.0-1.0,
     "depth_rank": <0~10 相对层序>, "label": "<人读名>"}
category 用 `decompose.CATEGORY_TO_KIND` 里的白名单标签（ground/slab/pillar/
lantern/prop/character/vfx/...），未命中的标签由确定性层降级 unknown。

⚠️ 深度用 `depth_rank`（相对层序，0=最远 -> N=最近），**不用 depth_m**：
eval_depth.py 已实证 VLM 单目「绝对米」不可信（同一物体跨轮漂移 3~80 倍，
越强调真实越编离谱），而「相对层序」完全可信。
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

# WorkBuddy 内部 VLM sidecar（IOA 授权，无需 API Key）
WORKBUDDY_VLM_ENDPOINT = "http://127.0.0.1:50669/skills/vlm/chat"
WORKBUDDY_VLM_MODEL = "kinfra-office-agent"

# 混元公网（备用通道，显式给 key 才用）
HUNYUAN_ENDPOINT = "https://api.hunyuan.cloud.tencent.com/v1/chat/completions"
HUNYUAN_VISION_MODEL = "hunyuan-vision"

# 白名单类别标签（与 decompose.CATEGORY_TO_KIND 的 key 对齐），写进 prompt
# 让模型尽量用这些标签，未命中也由确定性层兜底降级 unknown。
# 注意：必须用逗号分隔的独立词，不能用斜杠串——否则模型会把整串当成一个
# category 原样输出，导致 map_kind 全部落 unknown（实测踩坑）。
_CATEGORY_HINT = (
    "ground, floor, board, structure, pillar, wall, frame, prop, decoration, "
    "lantern, orb, furniture, character, hero, monster, pet, vfx, particle, effect"
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
    """构造让模型输出结构化检测结果的 prompt（含坐标/类别/层序约束）。"""
    return (
        "你是游戏场景分解器。请识别这张截图中所有独立的物体层（地面 / 结构 / "
        "道具 / 人物 / 特效），并按从远到近的层序输出一个 JSON 数组。\n"
        "\n"
        "图片尺寸：宽 %d 像素，高 %d 像素（bbox 用像素坐标，左上原点）。\n"
        "\n"
        "每个元素必须是如下 JSON 对象（不要输出任何其它文字，只输出 JSON 数组）：\n"
        "{\n"
        '  "bbox": [x0, y0, x1, y1],          // 像素坐标，0<=x0<x1<=宽，0<=y0<y1<=高\n'
        '  "category": "prop",               // 只用下面列表里的【一个词】：%s\n'
        '  "confidence": 0.9,                // 0.0-1.0，低于 0.5 的模糊物体也照实输出\n'
        '  "depth_rank": 5,                  // 相对层序 0~10：0=最远，10=最近，只表达远近顺序，不是米\n'
        '  "label": "朱红鼓车"                // 简短人读名，中文\n'
        "}\n"
        "\n"
        "规则：\n"
        "1. 地面 / 台面这种大块背景也作为一层，category 用 ground 或 board。\n"
        "2. 无法归类的物体 category 用 \"unknown\"。\n"
        "3. depth_rank 只用来表达从远到近的相对顺序（0=最远），不要估算米数。\n"
        "4. 只输出 JSON 数组本身，前后不要有任何解释文字、不要 markdown 代码块。\n"
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
    # 兼容模型吐出的不同字段名：prompt 要求 bbox，但部分 VLM 会吐 bbox_2d。
    bbox = item.get("bbox") if "bbox" in item else item.get("bbox_2d")
    if not isinstance(bbox, list) or len(bbox) != 4:
        raise VisionParseError("bbox 必须是 4 元素数组: %r" % str(item)[:100])
    category = str(item.get("category", "unknown")).strip()
    confidence = item.get("confidence")
    if confidence is None:
        confidence = 0.5
    confidence = float(confidence)
    # 优先 depth_rank（相对层序，可信）；depth_m 仅作兼容（绝对米，不可信）
    rank = item.get("depth_rank")
    rank = float(rank) if rank is not None else None
    depth = item.get("depth_m")
    depth = float(depth) if depth is not None else None
    return Detection(
        bbox=[float(v) for v in bbox],
        category=category,
        confidence=confidence,
        depth_rank=rank,
        depth_m=depth,
        label=item.get("label"),
    )


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
            return _jpeg_size(data)
    except Exception:
        pass
    return 1920, 1080


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


def _parse_sse_stream(body: str) -> str:
    """解析 OpenAI 兼容的 SSE 流，拼接 assistant 的 content 增量。

    形如：
        : heartbeat
        data: {"choices":[{"delta":{"content":"..."}}]}
        data: [DONE]

    返回拼接后的完整文本内容。若服务返回非流式 JSON（兜底），直接按
    choices[0].message.content 取。
    """
    text_parts: List[str] = []
    saw_data = False
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("data:"):
            continue
        saw_data = True
        chunk = line[5:].strip()
        if chunk == "[DONE]":
            continue
        try:
            obj = json.loads(chunk)
        except json.JSONDecodeError:
            continue
        # SSE chunk 结构
        try:
            delta = obj["choices"][0]["delta"]
            content = delta.get("content", "")
            if content:
                text_parts.append(content)
        except (KeyError, IndexError, TypeError):
            continue
    if text_parts:
        return "".join(text_parts)
    # 非流式兜底：整个 body 是一个 JSON 对象
    if not saw_data:
        try:
            obj = json.loads(body)
            return obj["choices"][0]["message"]["content"]
        except (json.JSONDecodeError, KeyError, IndexError, TypeError):
            pass
    return ""


class WorkBuddyVLMProvider(VisionProvider):
    """WorkBuddy 内部 VLM sidecar（IOA 授权，无需 API Key）。

    鉴权材料从环境变量读取（WorkBuddy 在 IOA 环境自动注入）：
      - WORKBUDDY_VLM_CHAT_URL   端点（缺省 127.0.0.1:50669/skills/vlm/chat）
      - WORKBUDDY_VLM_AUTH_TOKEN -> Authorization: Bearer
      - WORKBUDDY_VLM_CONTEXT    -> X-WorkBuddy-MCP-Context
      - WORKBUDDY_VLM_MODEL      模型名（缺省 kinfra-office-agent）

    服务只支持流式（stream: true）请求，返回 SSE 分片。
    """

    def __init__(self, endpoint: Optional[str] = None,
                 model: Optional[str] = None,
                 timeout: int = 120) -> None:
        self.endpoint = endpoint or os.environ.get(
            "WORKBUDDY_VLM_CHAT_URL", WORKBUDDY_VLM_ENDPOINT)
        self.model = model or os.environ.get(
            "WORKBUDDY_VLM_MODEL", WORKBUDDY_VLM_MODEL)
        self.auth_token = os.environ.get("WORKBUDDY_VLM_AUTH_TOKEN", "")
        self.vlm_context = os.environ.get("WORKBUDDY_VLM_CONTEXT", "")
        self.timeout = timeout
        if not self.auth_token or not self.vlm_context:
            raise RuntimeError(
                "缺少 WorkBuddy VLM 鉴权材料（WORKBUDDY_VLM_AUTH_TOKEN / "
                "WORKBUDDY_VLM_CONTEXT）。请确认在 WorkBuddy IOA 环境下运行。")

    def detect(self, image_path: str) -> List[Detection]:
        data_url = _load_image_base64(image_path)
        width, height = _probe_image_size(image_path)
        prompt = build_detection_prompt(width, height)

        body = {
            "model": self.model,
            "stream": True,
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
                "Authorization": "Bearer " + self.auth_token,
                "X-WorkBuddy-MCP-Context": self.vlm_context,
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            raise VisionParseError(
                "WorkBuddy VLM HTTP %d: %s" % (exc.code, detail[:500])) from exc

        content = _parse_sse_stream(raw)
        if not content:
            raise VisionParseError(
                "WorkBuddy VLM 返回空内容: %r" % raw[:500])

        items = _extract_json_array(content)
        return [_dict_to_detection(it) for it in items]


class HunyuanVisionProvider(VisionProvider):
    """混元 vision 的 OpenAI 兼容实现（备用通道，显式给 key 才用）。"""

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
        width, height = _probe_image_size(image_path)
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

        try:
            content = result["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise VisionParseError(
                "混元返回结构异常: %r" % str(result)[:500]) from exc

        items = _extract_json_array(content)
        return [_dict_to_detection(it) for it in items]


class MockVisionProvider(VisionProvider):
    """兜底 / 测试用：不调模型，直接返回 mock 检测结果。"""

    def detect(self, image_path: str) -> List[Detection]:
        return decompose.make_mock_detections()


def make_default_provider(model: Optional[str] = None) -> VisionProvider:
    """按环境选择默认 provider：优先 WorkBuddy IOA sidecar，否则 mock 兜底。

    - 若存在 WORKBUDDY_VLM_AUTH_TOKEN + WORKBUDDY_VLM_CONTEXT -> WorkBuddy
    - 否则 -> Mock（避免无鉴权材料时直接崩，方便离线跑链路）
    """
    if os.environ.get("WORKBUDDY_VLM_AUTH_TOKEN") and \
       os.environ.get("WORKBUDDY_VLM_CONTEXT"):
        return WorkBuddyVLMProvider(model=model)
    return MockVisionProvider()


def load_detections(path: str) -> List[Detection]:
    """读 detections JSON（与 decompose.load_detections 同口径，供调试）。"""
    return decompose.load_detections(path)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="L1 视觉理解：截图 -> Detection 列表")
    parser.add_argument("--image", required=True, help="截图路径")
    parser.add_argument("--out", required=True, help="输出 detections JSON 路径")
    parser.add_argument("--mock", action="store_true", help="用 mock provider，不调模型")
    parser.add_argument("--provider", default="workbuddy",
                        choices=["workbuddy", "hunyuan"],
                        help="provider：workbuddy（默认，IOA 无 key）/ hunyuan（需 HUNYUAN_API_KEY）")
    parser.add_argument("--model", default=None, help="覆盖模型名")
    args = parser.parse_args(argv)

    if args.mock:
        provider: VisionProvider = MockVisionProvider()
    elif args.provider == "hunyuan":
        provider = HunyuanVisionProvider(model=args.model or HUNYUAN_VISION_MODEL)
    else:
        provider = make_default_provider(model=args.model)

    detections = provider.detect(args.image)

    payload = [
        {
            "bbox": [float(v) for v in d.bbox],
            "category": d.category,
            "confidence": d.confidence,
            "depth_rank": d.depth_rank,
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
