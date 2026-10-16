"""
通用命令执行工具。这是「调用命令行一直等待」的正解。

为什么不能直接用 subprocess.run
-------------------------------
`zfault/process.py` 已经解决过一次子进程卡死，但它解决的是**崩溃进程**
这一个特例（WER 弹框 + 超时 + 杀进程树），且输出是一次性收回的。
agent 面对的是任意命令，卡住的成因更多，而且**必须区分成因**：
"超时"和"在等输入"对模型来说是完全不同的两件事 —— 前者该加长时限或
换实现，后者该换成非交互写法（`--no-pager`、`-y`、预设凭据）。
笼统回一句 `failed`，模型只能原样重试，于是**同一个命令卡死两次**。

卡住的五类成因与对策
--------------------
1. 根本没设超时            -> wall-clock `timeout`
2. 在等 stdin（git 要密码 / `rm -i` / 误进 REPL）
                           -> `stdin=DEVNULL`，让它立刻 EOF 而不是挂住
3. 分页器（`git log` 默认开 less、`man`）
                           -> env 注入 `PAGER=cat / GIT_PAGER=cat / TERM=dumb`
4. 只杀父进程，留下孤儿继续占资源
                           -> 杀进程树
5. 输出一次性收回，超时后什么都没剩下
                           -> 线程边跑边收，**超时也要保留已产出的部分**

第 5 条最容易被忽略，也最致命：编译失败时前 200 行就是错误信息，超时
被杀时前面已经吐出来的进度是唯一线索。丢掉它，模型就失去了唯一能据以
纠偏的材料，只能瞎猜重试 —— 而瞎猜重试正是"一直等待"的观感来源。

关于 stall_timeout 为什么默认关闭
---------------------------------
"长时间无输出"不等于"卡住"：g++ / clang++ 编译一个翻译单元几十秒不吐
一个字符是常态。所以判定条件收紧为「**从未**产出过任何输出」，并且默认
不开启 —— 误杀一个正在正常编译的任务，代价比多等一会儿大得多。
要开启它，前提是你明确知道目标命令会持续输出。
"""

from __future__ import annotations

import os
import re
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import IO, Callable

from .base import (STATUS_AWAITING_INPUT, STATUS_BAD_ARGS, STATUS_ERROR,
                   STATUS_KILLED, STATUS_NONZERO, STATUS_OK, STATUS_TIMEOUT,
                   Risk, Tool, ToolResult)

# 让常见工具不要进入交互/分页模式。
# 这几项不是"优化"，是**能否返回**的前提：git 拉不到凭据时会挂在那里等
# 终端输入，即使 stdin 是 DEVNULL（它走 /dev/tty）。GIT_TERMINAL_PROMPT=0
# 让它直接失败 —— 失败是模型可以处理的，挂住不是。
_QUIET_ENV: dict[str, str] = {
    "PAGER": "cat",
    "GIT_PAGER": "cat",
    "MANPAGER": "cat",
    "TERM": "dumb",
    "GIT_TERMINAL_PROMPT": "0",
}


# ---------------------------------------------------------------- 风险分类

# 命令内容决定风险等级，而不只是"这是个 shell 工具"就一律 EXEC。
# 分级错了的代价是不对称的：把 `ls` 判成高危只是多问一句，把
# `rm -rf` 判成普通执行是不可挽回的。所以宁可判高。
_DESTRUCTIVE_RE = re.compile(
    r"(?:^|\s|&&|;|\|)"
    r"(?:rm\s+.*-[rf]|rmdir\s+/[sq]|del\s+/[fq]|format\s+|diskpart|"
    r"git\s+clean\s+-[f]|git\s+reset\s+--hard|git\s+checkout\s+--\.)",
    re.IGNORECASE,
)

_IRREVERSIBLE_RE = re.compile(
    r"git\s+push\s+.*--force|git\s+push\s+.*\s-f\b|drop\s+database|"
    r"truncate\s+table|shutdown|reboot",
    re.IGNORECASE,
)

_NETWORK_RE = re.compile(
    r"(?:^|\s)(?:curl|wget|git\s+push|git\s+fetch|git\s+clone|pip\s+install|"
    r"npm\s+install|scp|rsync|ssh)\b",
    re.IGNORECASE,
)


def classify_command_risk(command: str) -> Risk:
    """按命令内容判风险。判不了高危就按 EXEC —— 保守方向。"""
    if _IRREVERSIBLE_RE.search(command):
        return Risk.IRREVERSIBLE
    if _DESTRUCTIVE_RE.search(command):
        return Risk.EXEC_DESTRUCTIVE
    if _NETWORK_RE.search(command):
        return Risk.NETWORK
    return Risk.EXEC


# ---------------------------------------------------------------- 结果

@dataclass
class ExecResult:
    """执行明细。除了给模型的文本，也保留程序可读的字段。"""

    command: str
    status: str = STATUS_OK
    returncode: int | None = None
    stdout: str = ""
    stderr: str = ""
    produced_output: bool = False     # 是否曾吐出过任何东西
    killed: bool = False
    elapsed_s: float = 0.0
    notes: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.status == STATUS_OK and self.returncode == 0


# ---------------------------------------------------------------- 执行核心

def _pump(stream: IO[str], sink: list[str], on_line: Callable[[], None]) -> None:
    """把子进程输出逐行搬到列表里。线程中运行。"""
    try:
        for line in iter(stream.readline, ""):
            sink.append(line)
            on_line()
    except (ValueError, OSError):
        pass
    finally:
        try:
            stream.close()
        except Exception:  # noqa: BLE001
            pass


def _kill_tree(p: "subprocess.Popen[str]") -> None:
    """
    杀掉进程及其子孙。

    与 `zfault.process._kill_tree` 的策略一致（psutil 优先，Windows 上退回
    `taskkill /T`）。这里复制一份而不是 import，是因为分层：`zagent` 不应
    依赖 `zfault` —— 后者是故障注入的实验设施，前者是要能独立跑在分析
    现场的库。若两边逻辑需要同步，应当把它下沉到公共模块，而不是让
    zagent 反向依赖实验设施。
    """
    try:
        p.kill()
    except Exception:  # noqa: BLE001
        pass

    try:
        import psutil  # type: ignore
        proc = psutil.Process(p.pid)
        for child in proc.children(recursive=True):
            try:
                child.kill()
            except Exception:  # noqa: BLE001
                pass
        return
    except Exception:  # noqa: BLE001
        pass

    if os.name == "nt":
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)],
                           capture_output=True, timeout=10, check=False)
        except Exception:  # noqa: BLE001
            pass


def _truncate(text: str, limit: int) -> tuple[str, bool]:
    """
    截断超长输出，**保留头尾**。

    为什么不能只留开头：编译器把真正的错误写在最后；测试框架把失败汇总
    写在最后。只留开头等于把结论丢掉、把过程留下。
    """
    if not limit or len(text) <= limit:
        return text, False
    keep = max(1, limit // 2 - 40)
    dropped = len(text) - 2 * keep
    return (
        text[:keep]
        + f"\n...[截断 {dropped} 字符]...\n"
        + text[-keep:],
        True,
    )


def execute(command: str,
            cwd: str | None = None,
            env: dict[str, str] | None = None,
            timeout: float = 60.0,
            stall_timeout: float | None = None,
            shell: bool = False,
            stdin_mode: str = "devnull") -> ExecResult:
    """
    执行一条命令，保证一定会返回，并说清是为什么返回的。

    stdin_mode
        "devnull"（默认）：子进程读 stdin 立刻得到 EOF。
        这是"在等输入"这类挂死的主要防线 —— 想吃输入的命令会立即失败，
        而失败是模型可以纠偏的，挂住不是。

    stall_timeout
        从未产出任何输出且超过该秒数 -> 判定为疑似等待输入（分页器 /
        tty 提示符），返回 `awaiting_input`。默认 None（关闭）。
    """
    res = ExecResult(command=command)
    start = time.monotonic()

    if cwd is not None and not Path(cwd).exists():
        res.status = STATUS_ERROR
        res.notes.append(f"工作目录不存在: {cwd}")
        return res

    full_env = os.environ.copy()
    full_env.update(_QUIET_ENV)
    if env:
        full_env.update(env)

    try:
        stdin = (subprocess.DEVNULL if stdin_mode == "devnull"
                 else subprocess.PIPE)
        p = subprocess.Popen(
            command if shell else _split_command(command),
            shell=shell,
            cwd=cwd,
            env=full_env,
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            bufsize=1,
        )
    except OSError as e:
        res.status = STATUS_ERROR
        res.notes.append(f"启动失败: {e}")
        return res

    out_buf: list[str] = []
    err_buf: list[str] = []
    last_out = [start]          # 可变容器，供闭包写入

    def _mark() -> None:
        now = time.monotonic()
        last_out[0] = now
        res.produced_output = True

    t_out = threading.Thread(target=_pump, args=(p.stdout, out_buf, _mark),
                             daemon=True)
    t_err = threading.Thread(target=_pump, args=(p.stderr, err_buf, _mark),
                             daemon=True)
    t_out.start()
    t_err.start()

    status = STATUS_OK
    # 只有真正需要中断才杀进程树。判据必须显式：
    # 早先写成 `if status != OK` 就杀，于是"跑完了但退出码非 0"也会被
    # 当成需要终止 —— 不但白杀一次，还可能连带杀掉命令正常拉起的后台
    # 子进程。跑完但失败，和跑不完，是两回事。
    need_kill = False
    try:
        while p.poll() is None:
            now = time.monotonic()
            if timeout and now - start > timeout:
                status = STATUS_TIMEOUT
                need_kill = True
                res.notes.append(f"超过 {timeout}s 未退出，已终止进程树")
                break
            if (stall_timeout and not res.produced_output
                    and now - start > stall_timeout):
                status = STATUS_AWAITING_INPUT
                need_kill = True
                res.notes.append(
                    f"启动后 {stall_timeout}s 未产出任何输出，"
                    f"疑似在等待输入或分页器")
                break
            time.sleep(0.02)

        if status == STATUS_OK:
            res.returncode = p.returncode
            # 跑完了但没成功 —— 与"成功"分开标注。
            # 混为一谈会让模型读到「[ok] 退出码 1」这种自相矛盾的观测。
            if res.returncode != 0:
                status = STATUS_NONZERO
    finally:
        res.status = status
        if need_kill:
            res.killed = True
            _kill_tree(p)
            try:
                p.wait(timeout=5)
            except Exception:  # noqa: BLE001
                pass

    t_out.join(timeout=2)
    t_err.join(timeout=2)

    res.stdout = "".join(out_buf)
    res.stderr = "".join(err_buf)
    res.elapsed_s = round(time.monotonic() - start, 3)
    return res


def _split_command(command: str) -> list[str]:
    """
    非 shell 模式下把命令串切成 argv。

    **为什么不用 shlex**：两条路都走不通。
    - `posix=True`：反斜杠被当转义符吃掉，`C:\\temp\\x.cpp` -> `C:tempx.cpp`。
      这套工具跑在 Windows 引擎工程里，路径里全是反斜杠，这会直接让
      "读这个文件"变成"读一个不存在的文件"，而且错得很隐蔽。
    - `posix=False`：不剥引号，`-c "print(1)"` 会把引号一起交给解释器，
      变成 `-c '"print(1)"'`，执行的是一个字符串字面量而不是那条语句。

    这里只需要"正确分词"，不需要完整 shell 语法：不处理管道、重定向、
    变量展开。需要那些能力时调用方应显式开 `shell=True`，而不是指望
    一个分词器假装自己是 shell。
    """
    out: list[str] = []
    cur: list[str] = []
    quote: str | None = None

    for ch in command:
        if quote:
            if ch == quote:
                quote = None
            else:
                cur.append(ch)
        elif ch in ('"', "'"):
            quote = ch
        elif ch.isspace():
            if cur:
                out.append("".join(cur))
                cur = []
        else:
            cur.append(ch)

    if cur:
        out.append("".join(cur))
    return out or [command]


# ---------------------------------------------------------------- 工具封装

def make_shell_tool(cwd: str | None = None,
                    timeout: float = 60.0,
                    stall_timeout: float | None = None,
                    max_output_chars: int = 8000,
                    env: dict[str, str] | None = None,
                    allow_shell: bool = False) -> Tool:
    """造一个 shell 工具实例。默认 cwd 与超时由构造参数固定。"""

    # 闭包外层默认值改别名，避免与工具入参同名后写出
    # `timeout=timeout if timeout is not None else timeout` 这种恒等式。
    default_cwd = cwd
    default_timeout = timeout

    def _shell(command: str,
               timeout: float | None = None,
               cwd: str | None = None,
               shell: bool | None = None) -> ToolResult:
        if not command or not command.strip():
            return ToolResult(ok=False, status=STATUS_BAD_ARGS,
                              error="command 为空")

        use_shell = allow_shell if shell is None else shell
        if use_shell and not allow_shell:
            return ToolResult(
                ok=False, status=STATUS_BAD_ARGS,
                error="该 shell 工具未开启 shell 模式（allow_shell=False）")

        r = execute(
            command=command,
            cwd=cwd or default_cwd,
            env=env,
            timeout=timeout if timeout is not None else default_timeout,
            stall_timeout=stall_timeout,
            shell=use_shell,
        )

        merged, truncated = _merge_output(r, max_output_chars)
        ok = r.status == STATUS_OK and r.returncode == 0

        return ToolResult(
            ok=ok,
            output=merged,
            error="" if ok else _error_text(r),
            status=r.status,
            truncated=truncated,
            elapsed_s=r.elapsed_s,
            data={
                "returncode": r.returncode,
                "produced_output": r.produced_output,
                "notes": list(r.notes),
            },
        )

    return Tool(
        name="shell",
        description=(
            "执行一条命令并返回输出。一定会返回，不会挂住。"
            "失败时会在 status 里说明原因（timeout / awaiting_input / error）；"
            "awaiting_input 表示命令在等输入，应改用非交互写法重试。"
        ),
        risk=Risk.EXEC,
        params={
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "要执行的命令"},
                "timeout": {"type": "number", "description": "超时秒数"},
                "cwd": {"type": "string", "description": "工作目录"},
            },
            "required": ["command"],
        },
        fn=_shell,
    )


def _merge_output(r: ExecResult, limit: int) -> tuple[str, bool]:
    """
    合并 stdout/stderr。

    **必须合并**：绝大多数工具把错误信息写 stderr，只回 stdout 会让模型
    看到"命令成功但什么也没输出"，从而得出完全错误的结论。顺序上 stderr
    放后面，因为它是"出了什么事"的答案。
    """
    text = r.stdout
    if r.stderr:
        text = (text + ("\n" if text and not text.endswith("\n") else "")
                + r.stderr)
    return _truncate(text, limit)


def _error_text(r: ExecResult) -> str:
    if r.status == STATUS_TIMEOUT:
        return f"执行超过时限被终止（退出码 {r.returncode}）"
    if r.status == STATUS_AWAITING_INPUT:
        return "命令疑似在等待输入或分页器，已终止"
    if r.status == STATUS_KILLED:
        return "进程被终止"
    if r.status == STATUS_NONZERO:
        return f"命令已执行完毕，但退出码为 {r.returncode}"
    if r.returncode is None:
        return "; ".join(r.notes) or "未能启动"
    return f"退出码 {r.returncode}"
