"""
崩溃进程执行器。

**为什么需要这个模块**

直接 `subprocess.run(crashy.exe)` 会卡死：进程崩溃后 Windows 错误报告（WER）
弹出「应用程序错误」对话框，该对话框不关闭，子进程就不退出，
父进程于是无限等待。这不是程序的 bug，是 Windows 的默认行为。

**解法：错误模式继承**

Windows 的 error mode 会被子进程继承。父进程调用
`SetErrorMode(SEM_NOGPFAULTERRORBOX)` 之后，
它启动的所有子进程都不会再弹 WER 对话框，而是直接终止。

这个方案优于改注册表（HKCU\...\Windows Error Reporting\DontShowUI）：
改注册表会影响用户整个机器的行为，属于副作用外溢；
继承 error mode 的作用域仅限本进程树，进程退出后自动失效。

**另一个必须处理的问题：超时**

崩溃进程也可能「挂住」而不是「崩掉」（例如栈溢出前先跑几分钟）。
所以每个子进程都要带超时，超时后连进程树一起杀掉，不能只杀自己。
"""

from __future__ import annotations

import ctypes
import subprocess
import time
from dataclasses import dataclass, field

# Windows error mode flags
SEM_FAILCRITICALERRORS = 0x0001
SEM_NOGPFAULTERRORBOX = 0x0002       # 不弹「应用程序错误」对话框
SEM_NOALIGNMENTFAULTEXCEPT = 0x0004
SEM_NOOPENFILEERRORBOX = 0x8000      # 不弹「找不到文件」对话框

# 常用的组合：屏蔽崩溃框 + 文件错误框
QUIET_ERROR_MODE = SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS

_error_mode_applied = False


def suppress_error_dialogs() -> bool:
    """
    设置本进程的错误模式，使其后续启动的子进程不再弹崩溃对话框。

    幂等，重复调用无副作用。非 Windows 平台直接返回 False。
    """
    global _error_mode_applied
    if _error_mode_applied:
        return True

    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.SetErrorMode.argtypes = [ctypes.c_uint]
        k32.SetErrorMode.restype = ctypes.c_uint
        k32.SetErrorMode(QUIET_ERROR_MODE)
        _error_mode_applied = True
        return True
    except (AttributeError, OSError):
        # 非 Windows
        return False


@dataclass
class RunResult:
    cmd: list[str]
    returncode: int | None = None
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False
    duration_s: float = 0.0
    killed: bool = False
    notes: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.returncode == 0

    def crashed(self) -> bool:
        """判断是否为异常终止（崩溃），而非正常退出。"""
        if self.timed_out or self.returncode is None:
            return False
        return self.returncode < 0 or self.returncode > 255

    def signal_desc(self) -> str:
        """把返回码翻译成人类可读的终止原因。"""
        if self.timed_out:
            return "超时被杀（可能是死循环或未触发崩溃）"
        if self.returncode is None:
            return "未知"
        rc = self.returncode
        if rc == 0:
            return "正常退出"
        if rc > 255 or rc < 0:
            code = rc & 0xFFFFFFFF
            known = {
                0xC0000005: "访问违例 (STATUS_ACCESS_VIOLATION)",
                0xC0000374: "堆破坏 (STATUS_HEAP_CORRUPTION)",
                0xC00000FD: "栈溢出 (STATUS_STACK_OVERFLOW)",
                0xC0000094: "整数除零 (STATUS_INTEGER_DIVIDE_BY_ZERO)",
                0xC0000409: "栈缓冲区溢出 / 安全检查失败 (STATUS_STACK_BUFFER_OVERRUN)",
                0xC000041D: "用户回调异常 (STATUS_FATAL_USER_CALLBACK_EXCEPTION)",
            }
            return known.get(code, f"异常终止 0x{code:08X}")
        return f"退出码 {rc}"


def run(cmd: list[str], timeout: float = 30.0,
        env: dict[str, str] | None = None,
        cwd: str | None = None,
        quiet: bool = True) -> RunResult:
    """
    运行一个可能会崩溃的子进程。

    保证：
      1. 不弹 WER 对话框（通过继承 error mode）
      2. 一定会返回（超时则杀掉进程树）
      3. 返回码可解释（crashed() / signal_desc()）
    """
    if quiet:
        suppress_error_dialogs()

    res = RunResult(cmd=list(cmd))
    start = time.monotonic()

    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=env, cwd=cwd, text=True, errors="replace")
    except OSError as e:
        res.returncode = None
        res.stderr = str(e)
        res.notes.append(f"启动失败: {e}")
        res.duration_s = time.monotonic() - start
        return res

    try:
        out, err = p.communicate(timeout=timeout)
        res.returncode = p.returncode
        res.stdout = out or ""
        res.stderr = err or ""
    except subprocess.TimeoutExpired:
        res.timed_out = True
        res.killed = True
        _kill_tree(p)
        try:
            out, err = p.communicate(timeout=5)
            res.stdout = out or ""
            res.stderr = err or ""
        except Exception:
            pass
        res.notes.append(f"超过 {timeout}s 未退出，已终止")

    res.duration_s = round(time.monotonic() - start, 3)
    return res


def _kill_tree(p: subprocess.Popen) -> None:
    """杀掉进程及其子进程。只杀自己会留下孤儿进程继续占用资源。"""
    try:
        p.kill()
    except Exception:
        pass

    try:
        import psutil  # type: ignore
        proc = psutil.Process(p.pid)
        for child in proc.children(recursive=True):
            try:
                child.kill()
            except Exception:
                pass
    except Exception:
        # 没有 psutil 时退回 taskkill（Windows）。
        # 注意不要去改用户的注册表或全局设置来"解决"这个问题。
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)],
                           capture_output=True, timeout=10)
        except Exception:
            pass
