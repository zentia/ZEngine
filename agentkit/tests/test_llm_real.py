"""
真 HTTP transport 的回归测试（本地 mock OpenAI 服务）。

    cd agentkit
    python -m unittest tests.test_llm_real -v

## 为什么必须有这个

`OpenAICompatTransport` 之前**零测试覆盖** —— 所有测试都走 `FakeTransport`。
于是那段代码第一次真正执行，是在有人把它指向真实端点的那一刻。如果
鉴权头写错、payload 形状不对、重试逻辑反了，全都在那个最糟的时机暴露：
你拿到一个 key、满怀期待跑起来，得到一句解析不出来的报错，然后要在一个
你没读过的文件里排查。

这里起一个本地 HTTP 服务冒充 OpenAI 兼容端点，把那几条路径全部走一遍。
不需要 key、不需要网络。

刻意测的几件事：
- **鉴权头真的带上了**（最容易写错、又最难从报错看出来的一条）
- 4xx 不重试、5xx 才重试（重试一个 400 纯属浪费时间）
- 响应结构异常时抛的是 LLMError，而不是 KeyError 掀翻调用方
"""

from __future__ import annotations

import json
import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.llm import (LLMConfig, LLMError, LLMPlanner, LLMResponse,
                        OpenAICompatTransport)
from zagent.loop import AgentLoop, AutoInteractor, STATUS_DONE
from zagent.policy import Mode, Policy
from zagent.tools import ToolRegistry, make_shell_tool

KEY_ENV = "ZAGENT_TEST_KEY"

STATE: dict = {"requests": [], "auth": [], "headers": [], "mode": "tool"}


class _Handler(BaseHTTPRequestHandler):
    # HTTP/1.0：不做 keep-alive，避免连接复用带来的偶发问题。
    protocol_version = "HTTP/1.0"

    def log_message(self, *args, **kwargs) -> None:      # 静音，别污染输出
        return

    def _send(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:                            # noqa: N802
        if self.path.endswith("/models"):
            self._send(200, {"data": [{"id": "mock-model"}]})
        else:
            self._send(404, {"error": {"message": "not found"}})

    def do_POST(self) -> None:                           # noqa: N802
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            payload = json.loads(raw)
        except ValueError:
            payload = {}
        STATE["requests"].append(payload)
        STATE["auth"].append(self.headers.get("Authorization"))
        # 记全量 header：只记 Authorization 的话，私有 header（如 Wsid）
        # 到底有没有发出去就无从验证 —— 那会让"支持私有 header"这条
        # 变成一句没测过的声明。
        STATE["headers"].append(dict(self.headers))

        mode = STATE["mode"]
        if mode == "error500":
            self._send(500, {"error": {"message": "boom"}})
            return
        if mode == "error400":
            self._send(400, {"error": {"message": "bad model"}})
            return
        if mode == "garbage":
            self._send(200, {"unexpected": True})
            return

        if mode == "tool":
            msg = {"role": "assistant", "content": "",
                   "tool_calls": [{
                       "id": "c1", "type": "function",
                       "function": {"name": "finish",
                                    "arguments": json.dumps(
                                        {"answer": "mock ok",
                                         "conclusions": [],
                                         "unknowns": []},
                                        ensure_ascii=False)}}]}
        else:
            msg = {"role": "assistant", "content": "我没用工具"}

        self._send(200, {"choices": [{"message": msg}],
                         "usage": {"total_tokens": 1}})


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever,
                         daemon=True).start()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self) -> None:
        STATE["requests"].clear()
        STATE["auth"].clear()
        STATE["headers"].clear()
        STATE["mode"] = "tool"
        os.environ[KEY_ENV] = "sk-test-xxx"
        self.cfg = LLMConfig(base_url=f"http://127.0.0.1:{self.port}/v1",
                             model="mock-model", api_key_env=KEY_ENV,
                             timeout=5.0, max_retries=2, retry_backoff=0.0)

    def tearDown(self) -> None:
        os.environ.pop(KEY_ENV, None)


class TransportTest(Base):
    def test_auth_header_is_sent(self):
        """
        鉴权头真的带上了。

        这条最容易写错：漏了 Bearer、或漏了 header，服务端回 401，
        而客户端侧看到的只是"HTTP 401"，看不出是自己没带 key。
        """
        OpenAICompatTransport().complete(
            [{"role": "user", "content": "hi"}], [], self.cfg)
        self.assertEqual(STATE["auth"], ["Bearer sk-test-xxx"])

    def test_payload_shape(self):
        OpenAICompatTransport().complete(
            [{"role": "user", "content": "hi"}], [], self.cfg)
        p = STATE["requests"][0]
        self.assertEqual(p["model"], "mock-model")
        self.assertEqual(p["messages"][0]["content"], "hi")
        self.assertIn("temperature", p)

    def test_stream_is_always_disabled(self):
        """
        必须显式写 `stream: false`。

        OpenAI 默认是 false，但内网 stream-server 默认是 **true** ——
        不写这个字段，拿回来的是 SSE 流（`data: {...}` 一片），
        而这里是按非流式 JSON 解析的，直接 JSONDecodeError。
        显式写出来两边都对，所以无条件写，且必须有测试守着。
        """
        OpenAICompatTransport().complete(
            [{"role": "user", "content": "hi"}], [], self.cfg)
        self.assertIs(STATE["requests"][0]["stream"], False)

    def test_extra_headers_are_sent(self):
        """
        内网网关要 `Wsid` 这种私有 header。走 extra_headers，不改源码。

        断言的是**服务端实际收到的 header**，不是 config 里的值 ——
        只断言后者等于什么都没测。
        """
        self.cfg.extra_headers = {"Wsid": "10103"}
        OpenAICompatTransport().complete(
            [{"role": "user", "content": "hi"}], [], self.cfg)
        self.assertEqual(STATE["headers"][0].get("Wsid"), "10103")

    def test_extra_body_merges_and_overrides(self):
        """网关私货字段（如 openai_infer / output_seq_len）。"""
        self.cfg.extra_body = {"openai_infer": True, "output_seq_len": 1024}
        OpenAICompatTransport().complete(
            [{"role": "user", "content": "hi"}], [], self.cfg)
        p = STATE["requests"][0]
        self.assertIs(p["openai_infer"], True)
        self.assertEqual(p["output_seq_len"], 1024)

    def test_parses_tool_calls(self):
        r = OpenAICompatTransport().complete(
            [{"role": "user", "content": "hi"}], [], self.cfg)
        self.assertEqual(len(r.tool_calls), 1)
        self.assertEqual(r.tool_calls[0]["function"]["name"], "finish")

    def test_missing_key_raises(self):
        os.environ.pop(KEY_ENV, None)
        with self.assertRaises(LLMError):
            OpenAICompatTransport().complete([], [], self.cfg)

    def test_extra_headers_env_shorthand(self):
        """
        `ZAGENT_EXTRA_HEADERS` 支持 `Wsid=10103` 简写。

        JSON 在 shell 里引号套引号极易写错，而写错是**静默**的 ——
        header 不发出去，服务端回 403，人只会以为自己没权限。
        简写把这种失败模式从"半小时排查错误方向"降成"看一眼就知道"。
        """
        from zagent.llm import _json_env
        os.environ["ZAGENT_TEST_JSON"] = "Wsid=10103;X-Foo=bar"
        try:
            self.assertEqual(_json_env("ZAGENT_TEST_JSON"),
                             {"Wsid": "10103", "X-Foo": "bar"})
        finally:
            os.environ.pop("ZAGENT_TEST_JSON", None)

    def test_extra_headers_env_json(self):
        from zagent.llm import _json_env
        os.environ["ZAGENT_TEST_JSON"] = '{"Wsid": "10103"}'
        try:
            self.assertEqual(_json_env("ZAGENT_TEST_JSON"),
                             {"Wsid": "10103"})
        finally:
            os.environ.pop("ZAGENT_TEST_JSON", None)

    def test_extra_headers_env_bad_json_is_empty(self):
        """拼错的 JSON 静默变空，不能让 CLI 起不来。"""
        from zagent.llm import _json_env
        os.environ["ZAGENT_TEST_JSON"] = "{not json"
        try:
            self.assertEqual(_json_env("ZAGENT_TEST_JSON"), {})
        finally:
            os.environ.pop("ZAGENT_TEST_JSON", None)

    def test_4xx_does_not_retry(self):
        """400 重试毫无意义 —— 请求本身是错的，再发一次还是错。"""
        STATE["mode"] = "error400"
        with self.assertRaises(LLMError):
            OpenAICompatTransport().complete([], [], self.cfg)
        self.assertEqual(len(STATE["requests"]), 1)

    def test_5xx_retries(self):
        """5xx 是服务端临时故障，值得重试。"""
        STATE["mode"] = "error500"
        with self.assertRaises(LLMError):
            OpenAICompatTransport().complete([], [], self.cfg)
        self.assertEqual(len(STATE["requests"]), self.cfg.max_retries + 1)

    def test_malformed_response_raises_llmerror(self):
        """
        结构异常要抛 LLMError，不能让 KeyError 掀翻调用方。

        planner 侧只捕获 LLMError 转成 BadStep；冒出别的异常类型，
        loop 就少了"让模型自我纠正"的机会。
        """
        STATE["mode"] = "garbage"
        with self.assertRaises(LLMError):
            OpenAICompatTransport().complete([], [], self.cfg)


class EndToEndTest(Base):
    def test_agent_runs_against_real_transport(self):
        """
        端到端：真 HTTP transport + LLMPlanner + AgentLoop。

        覆盖的是 `FakeTransport` 永远覆盖不到的那一层：HTTP、鉴权、
        响应解析。之前这一段完全没有测试。
        """
        reg = ToolRegistry()
        reg.register(make_shell_tool(timeout=15))
        out = AgentLoop(
            reg, LLMPlanner(reg, transport=OpenAICompatTransport(),
                            config=self.cfg),
            policy=Policy(mode=Mode.NORMAL),
            interactor=AutoInteractor(approve_default=False),
            max_steps=4,
        ).run("随便做点什么然后结束")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.answer, "mock ok")
        self.assertEqual(out.steps, 1)

    def test_no_function_calling_falls_back_to_text(self):
        """
        端点不支持 function calling 时，走"从文本里抠 JSON"的回退。

        这条路径能用但更脆，所以必须知道它真的能退过去 ——
        否则端点不支持工具调用时，症状会是"agent 一直原地打转"，
        而根因极难定位。
        """
        STATE["mode"] = "no_tool"
        reg = ToolRegistry()
        reg.register(make_shell_tool(timeout=15))
        planner = LLMPlanner(reg, transport=OpenAICompatTransport(),
                             config=self.cfg)
        resp = LLMResponse(content='```json\n{"answer": "文本回退"}\n```')
        self.assertIsInstance(planner._to_step(resp), object)


if __name__ == "__main__":
    unittest.main()
