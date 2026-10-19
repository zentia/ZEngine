"""
真实模型接入自检。

    cd agentkit
    set ZAGENT_API_KEY=sk-xxxxxxxx
    set ZAGENT_BASE_URL=https://api.hunyuan.cloud.tencent.com/v1
    set ZAGENT_MODEL=hunyuan-turbo
    python -m examples.llm_check

## 为什么要单独做这个

agent 能不能干活，**取决于端点支不支持 function calling**，不取决于它
能不能聊天。不支持的话 `LLMPlanner` 会回退到"从文本里抠 JSON"，
那条路能用但明显更脆（格式一变就解析失败，变成 BadStep）。

所以这里探的是能力，不是连通性：真发一个带 tools 的请求，看回的是
`tool_calls` 还是 `content`。这一步能省掉后面半天莫名其妙的排查 ——
很多"agent 一直在原地打转"的根因就是端点不支持工具调用而没人发现。
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.llm import LLMConfig

HUNYUAN = "https://api.hunyuan.cloud.tencent.com/v1"

PROBE_TOOL = {
    "type": "function",
    "function": {
        "name": "echo",
        "description": "把输入原样返回，用于确认工具调用链路可用",
        "parameters": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
}


def _get(url: str, key: str, timeout: float) -> tuple[int, str]:
    req = urllib.request.Request(
        url, headers={"Authorization": f"Bearer {key}"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return 0, f"{type(e).__name__}: {e}"


def _post(url: str, key: str, payload: dict,
          timeout: float) -> tuple[int, str]:
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers={"Authorization": f"Bearer {key}",
                 "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        return 0, f"{type(e).__name__}: {e}"


def _hint(code: int, body: str) -> str:
    """
    把状态码 + 错误正文翻译成"下一步该做什么"。

    **为什么必须有这层翻译**：不做的话，所有非 200 都会被笼统说成
    "模型名不对或 key 没权限"。实测踩到过 —— 拿一个余额为零的 key 跑，
    提示让我去换模型名，而换成列表里正确的名字依然报同样的错。真实原因
    （余额不足）被一句笼统提示盖掉，人就会去查一个根本没问题的方向。
    诊断工具给出误导性提示，比不给提示更糟。
    """
    msg = ""
    try:
        msg = str((json.loads(body).get("error") or {}).get("message", ""))
    except (ValueError, AttributeError):
        msg = ""
    prefix = f"服务端说：{msg}。\n      " if msg else ""

    table = {
        401: "key 无效或根本没带上。检查 Authorization 头是否发出去了。",
        402: "**账户余额不足** —— 不是模型名也不是 key 权限的问题，"
             "去控制台充值。",
        403: "key 无权访问该模型。换一个模型名，或申请权限。",
        404: "路径不对。检查 base_url：内部 stream-server 是 /openapi，"
             "公网一般是 /v1。",
        429: "触发限流。降低并发或稍后重试。",
    }
    if code in table:
        return f"{prefix}{table[code]}"
    if 500 <= code < 600:
        return f"{prefix}服务端故障，重试或稍后再试。"
    return (f"{prefix}HTTP {code}。若响应正文是 SSE 流（data: 一片），"
            f"说明服务端默认开了流式而忽略了 stream: false —— "
            f"这种情况客户端拿不到可解析的 JSON。")


def main() -> int:
    cfg = LLMConfig.from_env()
    key = os.environ.get(cfg.api_key_env, "")
    base = cfg.base_url.rstrip("/")

    print("=" * 66)
    print("真实模型接入自检")
    print("=" * 66)
    print(f"  base_url   {base}")
    print(f"  model      {cfg.model}")
    print(f"  key 来源    环境变量 {cfg.api_key_env}")

    if not key:
        print(f"\n[未配置] 环境变量 {cfg.api_key_env} 为空。\n")
        print("混元（已确认提供 OpenAI 兼容接口）：")
        print(f"  set ZAGENT_API_KEY=<控制台创建的 key>")
        print(f"  set ZAGENT_BASE_URL={HUNYUAN}")
        print(f"  set ZAGENT_MODEL=<模型名，见下面 /v1/models>")
        print("\n内部 stream-server（/openapi 那套）则要多两个变量：")
        print("  set ZAGENT_BASE_URL=http://<ip>:8080/openapi")
        print('  set ZAGENT_EXTRA_HEADERS={"Wsid":"10103"}')
        print("\n注意：混元的 API Key 与 CAM 密钥不是同一个东西，")
        print("      要在混元控制台 / TokenHub 单独创建。")
        print("\n没有 key 时 CLI 会自动退到 RulePlanner，不报错 ——")
        print("      所以这一步不会挡住你，只是拿不到真实模型的数字。")
        return 1

    print(f"  key        {key[:6]}...{key[-4:]} （已读取）\n")

    # 1) 连通 + 模型列表
    print("[1/3] GET /v1/models")
    code, body = _get(f"{base}/models", key, cfg.timeout)
    print(f"      HTTP {code}")
    if code == 200:
        try:
            ids = [m.get("id", "?") for m in json.loads(body).get("data", [])]
            print(f"      可用模型 {len(ids)} 个，前 10：")
            for m in ids[:10]:
                print(f"        - {m}")
            if cfg.model not in ids:
                print(f"\n      ** 当前 ZAGENT_MODEL={cfg.model} 不在列表里。"
                      f"挑一个上面的填进去。")
        except (ValueError, TypeError) as e:
            print(f"      响应解析失败: {e}")
            print(f"      {body[:300]}")
    else:
        print(f"      {body[:300]}")
        print("      拿不到列表不影响调用，但模型名只能自己确认。")

    # 2) function calling 能力 —— 这一步才是关键
    print("\n[2/3] POST /v1/chat/completions（带 tools，探 function calling）")
    payload = {
        "model": cfg.model,
        "messages": [{"role": "user",
                      "content": "调用 echo 工具，text 传 'ok'"}],
        "tools": [PROBE_TOOL],
        "tool_choice": "auto",
        "temperature": 0,
        "max_tokens": 256,
    }
    code, body = _post(f"{base}/chat/completions", key, payload, cfg.timeout)
    print(f"      HTTP {code}")

    if code != 200:
        print(f"      {_hint(code, body[:500])}")
        return 1

    try:
        msg = json.loads(body)["choices"][0]["message"]
    except (ValueError, KeyError, IndexError) as e:
        print(f"      响应结构无法解析: {e}\n      {body[:300]}")
        return 1

    calls = msg.get("tool_calls") or []
    if calls:
        name = calls[0].get("function", {}).get("name", "?")
        args = calls[0].get("function", {}).get("arguments", "")
        print(f"      **支持 function calling**：{name}({args})")
        print("      agent 可以走原生工具调用路径。")
    else:
        content = (msg.get("content") or "").strip()
        print(f"      **没有返回 tool_calls**，只回了 content：")
        print(f"      {content[:200]}")
        print("      agent 会回退到\"从文本里抠 JSON\"——能用，但更脆。")
        print("      如果这是预期外的，换一个模型试试。")

    # 3) 端到端：真跑一次 LLMPlanner
    print("\n[3/3] 端到端：LLMPlanner 真跑一步")
    try:
        from zagent.llm import LLMPlanner, OpenAICompatTransport
        from zagent.loop import (AgentLoop, AutoInteractor, STATUS_DONE,
                                 STATUS_STALLED)
        from zagent.policy import Mode, Policy
        from zagent.tools import ToolRegistry, make_shell_tool

        reg = ToolRegistry()
        reg.register(make_shell_tool(timeout=20))
        out = AgentLoop(
            reg, LLMPlanner(reg, transport=OpenAICompatTransport(),
                            config=cfg),
            policy=Policy(mode=Mode.NORMAL),
            interactor=AutoInteractor(approve_default=False),
            max_steps=4,
        ).run("用 shell 列出当前目录下的文件，然后结束。")

        print(f"      status={out.status}  steps={out.steps}")
        if out.status == STATUS_DONE:
            print(f"      answer: {(out.answer or '')[:200]}")
        elif out.status == STATUS_STALLED:
            print(f"      {out.answer[:200]}")
        print("\n[通过] 接真模型的链路是通的。")
        print("       下一步: python -m examples.run_eval_gate --real")
        return 0
    except Exception as e:  # noqa: BLE001
        print(f"      端到端失败: {type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
