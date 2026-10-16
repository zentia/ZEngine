"""
LLM 决策者：把 `Planner` 接到一个真正的模型上。

三条硬约束
----------
1. **零第三方依赖。** 用标准库 `urllib` 直连 OpenAI 兼容的
   `/v1/chat/completions`。不引入 openai / anthropic SDK —— 这套东西要能
   在只装了 Python 的机器上跑起来（CI 三平台、离线分析机）。

2. **必须可注入 transport。** `FakeTransport` 与 `ScriptedPlanner` 是同一
   个道理：接模型之前如果 loop 跑不通，出错时无法归因。更重要的是
   **eval 必须可复现** —— 没有可注入的假 transport，就没有确定性基线，
   "改了一版是不是变好了"这个问题就回答不了。

3. **失败降级为观测。** 网络错、超时、畸形 JSON、调用了不存在的工具，
   一律变成 `BadStep` 而不是异常。畸形输出是 LLM 最高频的故障，而且
   绝大多数可恢复 —— 模型看到"你上一步的 JSON 没闭合"就能自己改对。

工具调用的两条路径
------------------
- **原生 function calling**：优先。响应里的 `tool_calls` 直接转成 `Action`。
- **JSON 块回退**：模型没走 function calling 时，尝试从回复文本里解析
  一个 JSON 对象。这条路径是为了兼容不开 function calling 的部署
  （某些本地推理服务）。两条路径都不成立才视为 `Finish`。

保留键
------
`_thought` 与 `_confidence` 从工具实参里剥离，不传给工具本身：
前者进轨迹供复盘，后者进权限策略（`Policy` 会因低置信度转询问）。
注意这依赖服务端**非严格模式**容忍多余字段；若部署开了 strict
schema，需要换成让模型在单独的字段里输出。
"""

from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Protocol

from .loop import Action, AskUser, BadStep, Finish, Turn
from .tools.base import ToolRegistry

# 伪工具：不进注册表，由 planner 自己拦截。
# 做成"工具"而不是让模型输出特定格式，是因为模型对"调用一个叫 ask_user
# 的工具"远比"按某种格式写一段话"稳定 —— 前者它练过几百万次。
ASK_USER = "ask_user"
FINISH = "finish"

_RESERVED = ("_thought", "_confidence")

_ASK_USER_SCHEMA = {
    "name": ASK_USER,
    "description": "向用户提问。信息不足、或动作风险高而你没把握时使用。",
    "parameters": {
        "type": "object",
        "properties": {
            "question": {"type": "string"},
            "options": {"type": "array", "items": {"type": "string"}},
        },
        "required": ["question"],
    },
}

_FINISH_SCHEMA = {
    "name": FINISH,
    "description": "任务完成或无法继续时结束。unknowns 必须如实列出没查清的部分。",
    "parameters": {
        "type": "object",
        "properties": {
            "answer": {"type": "string"},
            "unknowns": {"type": "array", "items": {"type": "string"}},
            "confidence": {"type": "number"},
        },
        "required": ["answer"],
    },
}

SYSTEM_PROMPT = """你是一个严谨的排障 agent。每一步只能做一件事，看完观测再决定下一步。

工具使用：
- 需要更多信息就调工具，不要凭印象回答。没查过的地方不许出现在结论里。
- 命令返回 awaiting_input：说明命令在等输入或分页器，改用非交互写法重试
  （加 --no-pager / -y，或用非交互命令），不要原样重试。
- 命令返回 timeout：返回里会带上已产出的部分输出，那是唯一线索，先读它
  再决定是加长时限还是换实现。
- 命令返回 nonzero_exit：命令跑完了但失败了，读输出里的错误信息。

验证（硬约束）：
- 下判断前用 verify_hypothesis 验证。裁定由规则做出，不由你做出。
- **REFUTED 是有价值的结果**，说明这条假设不成立，换一条 ——
  不要当成"工具出错"去重试同一条。
- UNPROVEN 表示规则无法判定，这种假设**不许进结论**，但可以放进 unknowns。
- 只有"CONFIRMED 且带可回溯证据"的才允许写进结论。
- 收尾前调 conclusion_ledger 核一遍：结论里每条都要有 CONFIRMED 支撑。

诚实性要求（硬约束）：
- 没有证据支撑的判断不许写成结论。
- 被证据推翻的判断不许当成结论。
- 信息不足时：要么继续查，要么调 ask_user 问人。不许猜一个填上。
- 结束时用 finish，并在 unknowns 里如实列出没查清的部分 ——
  说"不知道"比给一个错误的结论有价值得多。

不确定性表达：
- 对自己这一步没把握时，把 _confidence 调低（0~1）。系统会据此决定要不要
  先问人。不要为了显得确定而写高。
"""


# ---------------------------------------------------------------- 传输层

@dataclass
class LLMConfig:
    model: str = "deepseek-chat"
    base_url: str = "https://api.deepseek.com/v1"
    api_key_env: str = "DEEPSEEK_API_KEY"
    timeout: float = 60.0
    temperature: float = 0.0      # eval 可复现的前提
    max_tokens: int = 2048
    max_retries: int = 2
    retry_backoff: float = 1.0


@dataclass
class LLMResponse:
    content: str = ""
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    usage: dict[str, Any] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)


class LLMError(Exception):
    pass


class LLMTransport(Protocol):
    """模型传输层。抽出来是为了可注入假实现（测试与可复现 eval）。"""

    def complete(self, messages: list[dict[str, Any]],
                 tools: list[dict[str, Any]],
                 config: LLMConfig) -> LLMResponse:
        ...


class OpenAICompatTransport:
    """
    OpenAI 兼容的 /v1/chat/completions 客户端（标准库实现）。

    覆盖 OpenAI / DeepSeek / Qwen / Moonshot / vLLM / Ollama 等所有走这套
    协议的部署 —— 这是事实标准，不做它就等于绑死一家供应商。
    """

    def complete(self, messages: list[dict[str, Any]],
                 tools: list[dict[str, Any]],
                 config: LLMConfig) -> LLMResponse:
        api_key = os.environ.get(config.api_key_env, "")
        if not api_key:
            raise LLMError(
                f"环境变量 {config.api_key_env} 未设置，无法调用模型")

        payload: dict[str, Any] = {
            "model": config.model,
            "messages": messages,
            "temperature": config.temperature,
            "max_tokens": config.max_tokens,
        }
        if tools:
            payload["tools"] = [
                {"type": "function", "function": t} for t in tools
            ]

        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            url=config.base_url.rstrip("/") + "/chat/completions",
            data=body,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {api_key}",
            },
            method="POST",
        )

        last: Exception | None = None
        for attempt in range(config.max_retries + 1):
            try:
                # 超时必须有：模型服务挂起是常态，而这里一挂就是整个
                # agent 卡死 —— 和 shell 那条"一定会返回"是同一个道理。
                with urllib.request.urlopen(req, timeout=config.timeout) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                return _parse_openai(data)
            except urllib.error.HTTPError as e:
                detail = ""
                try:
                    detail = e.read().decode("utf-8", "replace")[:300]
                except Exception:  # noqa: BLE001
                    pass
                last = LLMError(f"HTTP {e.code}: {detail}")
                if e.code < 500 and e.code != 429:
                    break            # 4xx 重试无意义，直接失败
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
                last = LLMError(f"{type(e).__name__}: {e}")

            if attempt < config.max_retries:
                time.sleep(config.retry_backoff * (2 ** attempt))

        raise last or LLMError("未知错误")


def _parse_openai(data: dict[str, Any]) -> LLMResponse:
    try:
        msg = data["choices"][0]["message"]
    except (KeyError, IndexError, TypeError) as e:
        raise LLMError(f"响应结构异常: {e}") from e

    return LLMResponse(
        content=msg.get("content") or "",
        tool_calls=list(msg.get("tool_calls") or []),
        usage=dict(data.get("usage") or {}),
        raw=data,
    )


class FakeTransport:
    """
    假传输层：按脚本返回响应。测试与可复现 eval 用。

    **它为什么是这个模块里最重要的东西**：真模型有随机性，用它跑出来的
    eval 数字无法复现，也就无法回答"改了一版是不是变好了"。有了可注入的
    transport，同一批响应可以稳定重放 —— 这正是 `ScriptedPlanner` 在
    loop 层、 `HeuristicReasoner` 在流水线层扮演的同一个角色。
    """

    def __init__(self, responses: list[LLMResponse | Exception]) -> None:
        self._responses = list(responses)
        self.calls: list[dict[str, Any]] = []

    def complete(self, messages: list[dict[str, Any]],
                 tools: list[dict[str, Any]],
                 config: LLMConfig) -> LLMResponse:
        self.calls.append({"messages": messages, "tools": tools})
        if not self._responses:
            raise LLMError("FakeTransport 脚本耗尽")
        item = self._responses.pop(0)
        if isinstance(item, Exception):
            raise item
        return item


# ---------------------------------------------------------------- planner

class LLMPlanner:
    """把模型接到 `Planner` 协议上。"""

    def __init__(self, registry: ToolRegistry,
                 transport: LLMTransport | None = None,
                 config: LLMConfig | None = None,
                 system_prompt: str = SYSTEM_PROMPT,
                 max_history_turns: int = 12,
                 max_observation_chars: int = 2000) -> None:
        self.registry = registry
        self.transport = transport or OpenAICompatTransport()
        self.config = config or LLMConfig()
        self.system_prompt = system_prompt
        # 只带最近 N 轮：轨迹会无限变长，而早期的失败尝试对当前决策几乎
        # 没有价值却占掉大量上下文。这是最省事的上下文管理，真正的
        # 压缩（摘要）留到后续。
        self.max_history_turns = max_history_turns
        self.max_observation_chars = max_observation_chars
        self.usage_log: list[dict[str, Any]] = []

    # ---- 构造请求 ----

    def _tools(self) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        for t in self.registry.schema():
            # risk 是给策略层看的，不是协议字段，别发给模型。
            out.append({"name": t["name"],
                        "description": t["description"],
                        "parameters": t.get("parameters", {})})
        out.append(_ASK_USER_SCHEMA)
        out.append(_FINISH_SCHEMA)
        return out

    def _messages(self, task: str, history: list[Turn],
                  pending: list[str]) -> list[dict[str, Any]]:
        msgs: list[dict[str, Any]] = [
            {"role": "system", "content": self.system_prompt},
            {"role": "user", "content": task},
        ]

        for t in history[-self.max_history_turns:]:
            if isinstance(t.step, Action):
                call_id = f"call_{t.index}"
                msgs.append({
                    "role": "assistant",
                    "content": t.step.thought or "",
                    "tool_calls": [{
                        "id": call_id,
                        "type": "function",
                        "function": {
                            "name": t.step.tool,
                            "arguments": json.dumps(t.step.args,
                                                    ensure_ascii=False),
                        },
                    }],
                })
                obs = t.observation.to_observation() if t.observation else "(无观测)"
                msgs.append({"role": "tool", "tool_call_id": call_id,
                             "content": _clip(obs, self.max_observation_chars)})
            elif isinstance(t.step, AskUser):
                msgs.append({"role": "assistant",
                             "content": f"[提问] {t.step.question}"})
                msgs.append({"role": "user",
                             "content": f"[回答] {t.answer or '(未回答)'}"})
            elif isinstance(t.step, BadStep):
                # 让模型看见自己上一步说了什么、以及为什么没法执行。
                msgs.append({"role": "assistant", "content": t.step.raw or ""})
                msgs.append({"role": "user",
                             "content": f"[系统] 上一步无法执行：{t.step.reason}。"
                                        f"请重新输出合法的工具调用。"})
            elif isinstance(t.step, Finish):
                msgs.append({"role": "assistant", "content": t.step.answer})

        if pending:
            # 中途插话：作为最新的用户消息注入，模型下一轮必然看到。
            msgs.append({"role": "user",
                         "content": "[用户插话] " + " / ".join(pending)})
        return msgs

    # ---- 解析响应 ----

    def _to_step(self, resp: LLMResponse) -> Action | Finish | AskUser | BadStep:
        if resp.tool_calls:
            return self._from_tool_call(resp.tool_calls[0])

        # 没走 function calling：尝试从文本里解析一个 JSON 块。
        parsed = _extract_json(resp.content)
        if isinstance(parsed, dict) and parsed.get("tool"):
            return self._from_tool_call(
                {"function": {"name": parsed.get("tool"),
                              "arguments": parsed.get("args", {})}})

        return _finish_from_text(resp.content)

    def _from_tool_call(self, call: Any) -> Action | Finish | AskUser | BadStep:
        # 不同部署的 tool_call 外层结构略有差异（有的直接给对象，有的给
        # 数组）。统一成"取第一个"，避免因为外壳不同就把可解析的调用
        # 判成畸形 —— 那会让 agent 在明明能干活的时候卡住。
        if isinstance(call, list):
            if not call:
                return BadStep(reason="tool_calls 为空")
            call = call[0]
        if not isinstance(call, dict):
            return BadStep(reason=f"tool_call 结构无法识别: {type(call).__name__}")

        fn = call.get("function") or call
        name = fn.get("name") or ""
        raw_args = fn.get("arguments")

        args: dict[str, Any]
        if isinstance(raw_args, dict):
            args = dict(raw_args)
        elif isinstance(raw_args, str):
            try:
                args = json.loads(raw_args) if raw_args.strip() else {}
            except json.JSONDecodeError as e:
                # 畸形 JSON：这是最高频故障，且几乎总是可恢复的。
                return BadStep(reason=f"工具实参不是合法 JSON: {e}",
                               raw=str(raw_args)[:400])
        else:
            args = {}

        if not isinstance(args, dict):
            return BadStep(reason=f"工具实参应为对象，收到 {type(args).__name__}",
                           raw=str(args)[:400])

        thought = str(args.pop("_thought", "") or "")
        try:
            confidence = float(args.pop("_confidence", 1.0))
        except (TypeError, ValueError):
            confidence = 1.0
        confidence = max(0.0, min(1.0, confidence))

        if name == ASK_USER:
            opts = args.get("options") or []
            return AskUser(question=str(args.get("question", "")),
                           options=[str(o) for o in opts] if isinstance(opts, list) else [])

        if name == FINISH:
            unk = args.get("unknowns") or []
            return Finish(
                answer=str(args.get("answer", "")),
                unknowns=[str(u) for u in unk] if isinstance(unk, list) else [],
                confidence=float(args.get("confidence", confidence) or confidence),
            )

        if not name:
            return BadStep(reason="工具调用缺少 name", raw=json.dumps(args)[:400])

        if self.registry.get(name) is None:
            return BadStep(
                reason=f"没有名为 {name!r} 的工具。可用: "
                       f"{', '.join(self.registry.names())}, {ASK_USER}, {FINISH}",
                raw=json.dumps(args)[:400])

        return Action(tool=name, args=args, thought=thought,
                      confidence=confidence)

    # ---- Planner 协议 ----

    def plan(self, task: str, history: list[Turn],
             pending: list[str]) -> Action | Finish | AskUser | BadStep:
        try:
            resp = self.transport.complete(
                self._messages(task, history, pending),
                self._tools(),
                self.config,
            )
        except LLMError as e:
            return BadStep(reason=f"模型调用失败: {e}")
        except Exception as e:  # noqa: BLE001 —— 传输层异常不许终结 loop
            return BadStep(reason=f"模型调用异常: {type(e).__name__}: {e}")

        if resp.usage:
            self.usage_log.append(dict(resp.usage))
        return self._to_step(resp)

    def stats(self) -> dict[str, Any]:
        """累计用量。eval 要算成本就靠它。"""
        total = {}
        for u in self.usage_log:
            for k, v in u.items():
                if isinstance(v, (int, float)):
                    total[k] = total.get(k, 0) + v
        return {"calls": len(self.usage_log), "usage": total}


def _clip(text: str, limit: int) -> str:
    if not limit or len(text) <= limit:
        return text
    return text[:limit] + f"\n...[截断 {len(text) - limit} 字符]"


def _extract_json(text: str) -> Any:
    """从文本里抠出第一个 JSON 对象。失败返回 None，不抛。"""
    if not text:
        return None
    fenced = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    raw = fenced.group(1) if fenced else None
    if raw is None:
        start = text.find("{")
        if start < 0:
            return None
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    raw = text[start:i + 1]
                    break
    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return None


def _finish_from_text(text: str) -> Finish:
    """
    把收尾文本转成 Finish。

    优先按 {"answer", "unknowns", "confidence"} 解析；解析不出来就整段当
    answer —— 但那种情况下 unknowns 是空的，置信度无从判断，这本身就是
    一个信号：模型没有按约定收尾。
    """
    parsed = _extract_json(text)
    if isinstance(parsed, dict) and "answer" in parsed:
        unk = parsed.get("unknowns") or []
        try:
            conf = float(parsed.get("confidence", 1.0))
        except (TypeError, ValueError):
            conf = 1.0
        return Finish(answer=str(parsed["answer"]),
                      unknowns=[str(u) for u in unk] if isinstance(unk, list) else [],
                      confidence=conf)
    return Finish(answer=(text or "").strip())
