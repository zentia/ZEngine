"""
交互式 CLI。这是这套东西的**入口**，不是崩溃诊断的入口。

    cd agentkit
    python -m zagent.cli                      # 进入交互
    python -m zagent.cli --task "诊断 xxx"     # 单次执行，跑完退出（CI 用）

定位：一个可交互的命令行 agent。崩溃诊断是它注册进来的一个能力；
今天可以再加"查编译日志""跑实验台"，结构不用动。

三条设计取向
------------
1. **没有模型也要能用。** 未配置 API key 时退到 `RulePlanner`：一个规则
   路由器，能派发诊断、能提问，但**明确标注自己是规则驱动**。
   为什么不做成"没 key 就报错退出"：那会让这个 CLI 在离线分析机、在 CI
   上都变成死文件。而"降级后仍可用，但说清自己在降级"，比"要么全有要么
   全无"有用得多。

2. **执行中可以插话。** 任务跑在后台线程，主线程同时读键盘：
   - 有话要说 -> 立即注入（steering），决策者下一轮读到；
   - agent 提问 -> 主线程接过来问人，agent 阻塞等答案。
   没有这条通道，"交互"就只剩一问一答，那跟脚本没区别。

3. **每一步都打印出来。** 用户盯着黑屏等 30 秒，和看着 30 秒的逐步输出，
   是两种完全不同的信任。这里靠 `AgentLoop` 的 `on_turn` 回调实现，
   loop 本身不知道自己在被 CLI 驱动。

斜杠命令
--------
    /help            帮助
    /tools           列出可用工具与风险等级
    /mode <档位>     plan | normal | autopilot
    /trace           打印上一轮的轨迹摘要
    /exit            退出
"""

from __future__ import annotations

import argparse
import queue
import sys
import threading
from pathlib import Path
from typing import Any, Callable

from .adapters import (GitCodeIndex, LocalDumpSource, SimpleVersionResolver,
                       find_git)
from .llm import (FakeTransport, LLMConfig, LLMPlanner, OpenAICompatTransport)
from .protocols import Context
from .verify import HeuristicReasoner, RuleVerifier
from .loop import (Action, AgentLoop, AskUser, BadStep, Finish, Interactor,
                   Planner, RunOutcome, ScriptedPlanner, Turn,
                   STATUS_DONE, STATUS_MAX_STEPS, STATUS_STALLED)
from .policy import Mode, Policy
from .tools import Risk, ToolRegistry, make_code_tools, make_shell_tool
from .tools.crash import make_crash_tools
from .tools.verify_tool import make_verify_tools

PROMPT = "you> "

BANNER = """\
ZEngine Agent — 可交互命令行 agent
  崩溃诊断是其中一个能力，不是全部。
  输入 /help 看命令；执行过程中随时可以直接说话打断。
"""


# ---------------------------------------------------------------- 装配

def build_registry(root: Path,
                   resolve_crash_ctx: Callable[[str], Any] | None = None,
                   trace_dir: Path | None = None,
                   allow_shell: bool = True) -> ToolRegistry:
    """按当前环境装配工具集。有什么装什么，缺什么就少什么。"""
    reg = ToolRegistry()
    reg.register(make_shell_tool(cwd=str(root), timeout=60,
                                 allow_shell=allow_shell))

    # 代码工具：只在是 git 仓库时才有意义。
    # 装了但底下没有 git，每次调用都返回空 —— 那比不装更糟，
    # 因为模型会以为"查过了，没有"，从而得出错误结论。
    if find_git() is not None and (root / ".git").exists():
        code = GitCodeIndex(root)
        for t in make_code_tools(code):
            reg.register(t)
        # 验证工具与代码工具**共用同一个 code 索引**：
        # 两套索引可能指向不同的工作树/版本，规则读到的代码和
        # 模型看到的代码就会不一致 —— 那种不一致极难发现，
        # 因为两边各自都是自洽的。
        for t in make_verify_tools(code, RuleVerifier()):
            reg.register(t)

    if resolve_crash_ctx is not None:
        for t in make_crash_tools(resolve_crash_ctx, trace_dir=trace_dir):
            reg.register(t)

    return reg


# ---------------------------------------------------------------- 离线路由

class RulePlanner:
    """
    规则路由器。**离线兜底，不是 agent 的全部。**

    它存在的意义是让 CLI 在没有模型时仍然可用，但它只有一步：派发一次
    工具调用就收尾。真正的多步推理要接 `LLMPlanner`。

    所以它在收尾时**如实声明**这一点，并把"没查清的部分"放进 unknowns ——
    这跟系统整体的诚实性约束是同一条规则，降级时也不能例外。
    """

    CRASH_WORDS = ("崩溃", "crash", "dump", "定位", "minidump")

    def __init__(self, registry: ToolRegistry) -> None:
        self.registry = registry

    def plan(self, task: str, history: list[Turn],
             pending: list[str]) -> Action | Finish | AskUser:
        if not history:
            return self._route(task)

        last = history[-1]
        obs = last.observation
        if obs is None:
            return Finish(answer="(无观测)")

        if obs.ok:
            return Finish(
                answer=obs.output[:2000],
                unknowns=["规则路由只执行了一步，未做多步推理；"
                          "未配置模型时无法继续深入"])

        return Finish(
            answer="这一步没能拿到结果。",
            unknowns=[obs.error or obs.output[:300],
                      "规则路由只有一步，不会自行换方法重试"])

    def _route(self, task: str) -> Action | Finish | AskUser:
        low = task.lower()
        if any(w in low for w in self.CRASH_WORDS):
            if "diagnose_crash" not in self.registry.names():
                return Finish(answer="未装配崩溃诊断能力",
                              unknowns=["崩溃诊断需要注入 resolve_crash_ctx"])
            ref = self._extract_ref(task)
            if not ref:
                return AskUser(question="请给出崩溃 ref（dump 文件路径或崩溃 ID）")
            return Action(tool="diagnose_crash", args={"ref": ref},
                          thought="命中崩溃关键词，派发诊断工具",
                          confidence=0.7)
        return Finish(
            answer="离线规则路由无法处理这条请求。",
            unknowns=["需要配置模型（DEEPSEEK_API_KEY 等）才能做多步推理",
                      f"当前可用工具: {', '.join(self.registry.names())}"])

    @staticmethod
    def _extract_ref(task: str) -> str:
        """从用户输入里抠出 ref：路径样式的 token 优先。"""
        for tok in task.replace("，", " ").split():
            tok = tok.strip("\"'“”`(),")
            if any(ch in tok for ch in "/\\") or tok.lower().endswith(
                    (".json", ".dmp", ".mdmp")):
                return tok
        return ""


# ---------------------------------------------------------------- 交互

class QueueInteractor:
    """
    把 agent 的提问交给主线程去问人。

    **为什么不能直接 `input()`**：agent 跑在后台线程，而主线程正阻塞在
    `input()` 上读键盘。两个线程同时读 stdin 会互相吃掉对方的输入。
    所以这里改成队列 + 事件：agent 阻塞等答案，主线程统一读键盘并回传。
    """

    def __init__(self) -> None:
        self._q: "queue.Queue[tuple[str, dict]]" = queue.Queue()

    def ask(self, req: AskUser) -> str:
        slot: dict[str, Any] = {"event": threading.Event()}
        self._q.put((req.question, slot))
        slot["event"].wait()
        return str(slot.get("answer", ""))

    def approve(self, question: str) -> bool:
        ans = self.ask(AskUser(question=question, options=["是", "否"]))
        return ans.strip().lower() in ("y", "yes", "是", "1", "ok", "同意")

    def pending(self) -> tuple[str, dict] | None:
        try:
            return self._q.get_nowait()
        except queue.Empty:
            return None

    @staticmethod
    def reply(slot: dict, answer: str) -> None:
        slot["answer"] = answer
        slot["event"].set()


class _Reader(threading.Thread):
    """后台读键盘。专门开一条线程，主线程才能一边等输入一边盯进度。"""

    def __init__(self, q: "queue.Queue[str | None]", stop: threading.Event) -> None:
        super().__init__(daemon=True)
        self.q = q
        self.stop = stop

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                line = input()
            except (EOFError, KeyboardInterrupt):
                self.q.put(None)
                return
            self.q.put(_clean(line))


def _clean(s: str) -> str:
    """
    去掉 BOM 与空白。

    BOM 不是臆想：从管道/重定向喂输入时（CI、`type file | python -m ...`）
    首行常带 `\\ufeff`，一旦带上，用户敲的 `/exit` 就不再以 `/` 开头，
    **被当成一句普通任务去执行** —— 表现是"我敲了命令但它跑了别的"，
    而根因只是一个不可见字符。这类问题排查成本极高，所以在入口一次性剥掉。
    """
    return s.replace("\ufeff", "").strip()


# ---------------------------------------------------------------- 会话

class Session:
    def __init__(self, root: Path, mode: Mode = Mode.NORMAL,
                 resolve_crash_ctx: Callable[[str], Any] | None = None,
                 trace_dir: Path | None = None,
                 planner: Planner | None = None,
                 max_steps: int = 12,
                 use_llm: bool = True) -> None:
        self.root = root
        self.mode = mode
        self.registry = build_registry(root, resolve_crash_ctx, trace_dir)
        self.last: RunOutcome | None = None

        self._planner: Planner
        if planner is not None:
            self._planner = planner
        elif use_llm and _has_key():
            self._planner = LLMPlanner(self.registry,
                                       transport=OpenAICompatTransport(),
                                       config=LLMConfig())
        else:
            self._planner = RulePlanner(self.registry)

        self._loop = AgentLoop(
            self.registry, self._planner,
            policy=Policy(mode=mode),
            interactor=QueueInteractor(),
            max_steps=max_steps,
            on_turn=self._print_turn,
        )

    @property
    def planner_kind(self) -> str:
        return "LLM" if isinstance(self._planner, LLMPlanner) else "规则路由"

    # ---- 单次执行（CI / 非交互） ----

    def run_once(self, task: str) -> RunOutcome:
        """一次性跑完。用 `AutoInteractor`：**非交互场景下默认不同意**。

        一个默认同意一切的实现，会让"权限策略生效了吗"在 CI 里永远测不出来。
        """
        self._loop.interactor = _NonInteractive()
        out = self._loop.run(task)
        self.last = out
        return out

    # ---- 交互 ----

    def _print_turn(self, t: Turn) -> None:
        s = t.step
        if isinstance(s, Action):
            head = f"  · {s.tool}"
            if s.thought:
                head += f"  ({s.thought})"
            print(head)
            if t.observation:
                body = t.observation.to_observation()
                for line in body.splitlines()[:12]:
                    print(f"      {line}")
                if len(body.splitlines()) > 12:
                    print("      ...")
        elif isinstance(s, BadStep):
            print(f"  ! 无法执行: {s.reason}")
        elif isinstance(s, AskUser):
            print(f"  ? {s.question}")

    def start(self, task: str) -> None:
        def _work() -> None:
            try:
                self.last = self._loop.run(task)
            except Exception as e:  # noqa: BLE001 —— 后台线程异常不能静默
                print(f"\n[任务异常] {type(e).__name__}: {e}")
                self.last = None
            finally:
                self._done.set()

        self._done = threading.Event()
        threading.Thread(target=_work, daemon=True).start()

    def interactive(self) -> int:
        print(BANNER)
        print(f"工作目录: {self.root}")
        print(f"决策者:   {self.planner_kind}")
        print(f"档位:     {self.mode.value}\n")

        line_q: "queue.Queue[str | None]" = queue.Queue()
        stop = threading.Event()
        _Reader(line_q, stop).start()

        busy = False
        while True:
            # 先服务 agent 的提问：它正阻塞着等答案，不能让用户干等。
            pend = self._loop.interactor.pending()  # type: ignore[union-attr]
            if pend is not None:
                question, slot = pend
                print(f"\n[agent 提问] {question}")
                print("you(答)> ", end="", flush=True)
                QueueInteractor.reply(slot, self._wait_line(line_q) or "")
                continue

            try:
                line = line_q.get(timeout=0.2)
            except queue.Empty:
                if busy and self._done.is_set():
                    busy = False
                    self._report()
                    print(PROMPT, end="", flush=True)
                continue

            if line is None:
                break
            text = _clean(line)
            if not text:
                continue

            # 斜杠命令要在"是否忙碌"之前判：任务跑着的时候用户敲 /exit
            # 是想退出，不是想给 agent 插一句 "/exit"。
            if text.startswith("/"):
                if self._command(text) == 1:
                    break
                print(PROMPT, end="", flush=True)
                continue

            if busy:
                # 任务执行中说话 -> 立即注入，决策者下一轮读到
                self._loop.push(text)
                print(f"  ^ 已注入: {text}")
                continue

            busy = True
            self.start(text)
            print(f"\n[任务开始] {text}")

        stop.set()
        return 0

    def _wait_line(self, line_q: "queue.Queue[str | None]") -> str | None:
        while True:
            item = line_q.get()
            if item is None:
                return None
            item = _clean(item)
            if item.startswith("/"):
                self._command(item)
                continue
            return item

    def _report(self) -> None:
        out = self.last
        if out is None:
            return
        print()
        if out.status != STATUS_DONE:
            print(f"[未正常结束] 状态={out.status}")
        print(f"\n{out.answer}")
        if out.unknowns:
            print("\n没查清的部分:")
            for u in out.unknowns:
                print(f"  - {u}")
        print()

    def _command(self, text: str) -> int:
        """返回 1 表示退出。"""
        cmd, _, arg = text.partition(" ")
        cmd = cmd.lower()
        arg = arg.strip()

        if cmd in ("/exit", "/quit", "/q"):
            return 1
        if cmd == "/help":
            print(__doc__ or "")
        elif cmd == "/tools":
            for t in self.registry.schema():
                print(f"  {t['name']:<16} [{t.get('risk','')}] {t['description'][:50]}")
        elif cmd == "/mode":
            try:
                self.mode = Mode(arg or self.mode.value)
                self._loop.policy.mode = self.mode
                print(f"档位 -> {self.mode.value}")
            except ValueError:
                print(f"未知档位: {arg}（可选 plan / normal / autopilot）")
        elif cmd == "/trace":
            if self.last is None:
                print("  (还没有跑过任务)")
            else:
                for t in self.last.turns:
                    kind = type(t.step).__name__
                    name = getattr(t.step, "tool", "")
                    print(f"  [{t.index}] {kind:<8} {name}")
        else:
            print(f"未知命令: {cmd}（/help 看全部）")
        return 0


class _NonInteractive(Interactor):
    """非交互：一律不同意、不回答。默认同意会掩盖权限问题。"""

    def ask(self, req: AskUser) -> str:
        return ""

    def approve(self, question: str) -> bool:
        return False


def _has_key() -> bool:
    import os
    return bool(os.environ.get("DEEPSEEK_API_KEY")
                or os.environ.get("ZAGENT_API_KEY")
                or os.environ.get("OPENAI_API_KEY"))


# ---------------------------------------------------------------- 入口

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="zagent", description=__doc__.split("\n")[1])
    ap.add_argument("--root", default=".", help="工作目录")
    ap.add_argument("--mode", default="normal",
                    choices=[m.value for m in Mode])
    ap.add_argument("--task", help="单次执行后退出（不进交互）")
    ap.add_argument("--no-llm", action="store_true", help="强制使用规则路由")
    ap.add_argument("--max-steps", type=int, default=12)
    ap.add_argument("--dump", help="崩溃 dump 文件；给了才装配崩溃诊断能力")
    args = ap.parse_args(argv)

    root = Path(args.root).resolve()
    if not root.exists():
        print(f"工作目录不存在: {root}")
        return 2

    resolve_crash_ctx = None
    if args.dump:
        dump = Path(args.dump).resolve()
        if not dump.exists():
            print(f"dump 不存在: {dump}")
            return 2
        code = GitCodeIndex(root)
        reasoner = HeuristicReasoner(code)

        def resolve_crash_ctx(ref: str) -> Context:  # noqa: F811
            # ref 为空时兜底用 --dump，否则按 ref 定位。
            # 这样 crash_ready 这种不带 ref 的自检也能工作。
            p = Path(ref) if ref else dump
            return Context(source=LocalDumpSource(p if p.exists() else dump),
                           version=SimpleVersionResolver(),
                           code=code, reasoner=reasoner,
                           verifier=RuleVerifier())

    sess = Session(root, mode=Mode(args.mode), max_steps=args.max_steps,
                   use_llm=not args.no_llm,
                   resolve_crash_ctx=resolve_crash_ctx)

    if args.task:
        out = sess.run_once(args.task)
        print(out.answer)
        if out.unknowns:
            print("\n没查清的部分:")
            for u in out.unknowns:
                print(f"  - {u}")
        return 0 if out.ok else 1

    return sess.interactive()


if __name__ == "__main__":
    raise SystemExit(main())
