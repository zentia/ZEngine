"""
agent 工具层与 loop 的回归测试。

    cd agentkit
    python -m unittest tests.test_agent_tools -v

测的是三件事，对应三个痛点：
1. 命令一定会返回，且说清是为什么返回的（不再"一直等待"）
2. 风险分级真的拦得住动作（不再是全局开关）
3. loop 不会无限跑（max_steps / 停滞检测 / 重复检测）
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.loop import (STATUS_DONE, STATUS_MAX_STEPS, STATUS_STALLED,
                         Action, AgentLoop, AskUser, AutoInteractor, Finish,
                         ScriptedPlanner)
from zagent.policy import Mode, Policy
from zagent.tools import Risk, ToolRegistry
from zagent.tools.base import STATUS_BAD_ARGS, STATUS_DENIED
from zagent.tools.code import make_code_tools
from zagent.tools.shell import _split_command, classify_command_risk, execute, make_shell_tool

PY = sys.executable


class ShellBasics(unittest.TestCase):
    def test_normal_command(self):
        r = execute(f'"{PY}" -c "print(123)"', timeout=20)
        self.assertEqual(r.status, "ok")
        self.assertEqual(r.returncode, 0)
        self.assertIn("123", r.stdout)

    def test_nonexistent_cwd(self):
        r = execute(f'"{PY}" -c "print(1)"', cwd="Z:/no/such/dir")
        self.assertEqual(r.status, "error")
        self.assertTrue(r.notes)


class ShellNeverHangs(unittest.TestCase):
    """痛点 1：命令一定会返回。"""

    def test_timeout_returns(self):
        r = execute(f'"{PY}" -c "print(1)"', timeout=0.01)
        self.assertEqual(r.status, "timeout")
        self.assertTrue(r.killed)

    def test_timeout_keeps_partial_output(self):
        """
        最关键的一条：超时也要把已吐出的部分带回来。

        编译跑到一半被杀时，前面那些输出是唯一的纠偏线索。丢了它，模型
        只能原样重试 —— 而原样重试正是"一直等待"的观感来源。
        """
        cmd = (f'"{PY}" -c "import sys,time; print(\'BEGIN\'); '
               f'sys.stdout.flush(); time.sleep(30)"')
        r = execute(cmd, timeout=3)
        self.assertEqual(r.status, "timeout")
        self.assertIn("BEGIN", r.stdout, "部分输出必须保留")

    def test_waiting_on_stdin_does_not_hang(self):
        """
        stdin=DEVNULL：想吃输入的命令立刻 EOF 而不是挂住。

        这是"在等输入"类挂死的主要防线 —— 失败是模型可以纠偏的，挂住不是。
        """
        r = execute(f'"{PY}" -c "input()"', timeout=20)
        self.assertNotEqual(r.status, "timeout")
        self.assertEqual(r.returncode, 1)      # EOFError

    def test_stall_timeout_detects_suspected_wait(self):
        """从未产出任何输出 -> 判定为疑似等待输入，而不是笼统的 timeout。"""
        cmd = f'"{PY}" -c "import time; time.sleep(30)"'
        r = execute(cmd, timeout=60, stall_timeout=1.5)
        self.assertEqual(r.status, "awaiting_input",
                         "无输出且被判定为疑似等待，应返回 awaiting_input")
        self.assertFalse(r.produced_output)

    def test_stall_timeout_does_not_kill_working_command(self):
        """
        有输出的命令不该被 stall 判定误杀。

        这条是"为什么 stall_timeout 默认关闭"的正面证据：它只在"从未
        产出输出"时才触发，正在正常吐日志的长任务不受影响。
        """
        cmd = (f'"{PY}" -c "import sys,time; '
               f'[print(i, flush=True) or time.sleep(0.3) for i in range(6)]"')
        r = execute(cmd, timeout=30, stall_timeout=1.0)
        self.assertEqual(r.status, "ok")
        self.assertTrue(r.produced_output)


class ShellToolResult(unittest.TestCase):
    def setUp(self):
        self.tool = make_shell_tool(timeout=20)

    def test_stderr_is_merged(self):
        """
        错误信息写 stderr，只回 stdout 会让模型以为命令成功但无输出。

        注意这里退出码是 0 —— 往 stderr 写字并不改变退出码，所以 r.ok 为
        True 是对的。要验的是**内容被带回来了**，而不是它被判成失败。
        """
        r = self.tool.fn(command=f'"{PY}" -c "import sys; sys.stderr.write(\'BOOM\')"')
        self.assertIn("BOOM", r.output)

    def test_empty_command(self):
        r = self.tool.fn(command="   ")
        self.assertEqual(r.status, STATUS_BAD_ARGS)

    def test_nonzero_exit_is_not_ok(self):
        """
        跑完了但失败，不能标成 ok。

        否则模型会读到「[ok] 退出码 1」这种自相矛盾的话，从而把失败当
        成功往下推 —— 这正是"观察到了但没看懂"的典型形态。
        """
        r = self.tool.fn(command=f'"{PY}" -c "import sys; sys.exit(3)"')
        self.assertFalse(r.ok)
        self.assertEqual(r.status, "nonzero_exit")
        self.assertEqual(r.data["returncode"], 3)
        self.assertNotIn("[ok]", r.to_observation())

    def test_successful_command_reports_ok(self):
        r = self.tool.fn(command=f'"{PY}" -c "print(1)"')
        self.assertTrue(r.ok)
        self.assertEqual(r.status, "ok")

    def test_truncation_keeps_head_and_tail(self):
        """编译器把错误写在最后，只留开头等于丢掉结论。"""
        from zagent.tools.shell import _truncate
        text = "".join(f"{i:05d}\n" for i in range(500))   # 3000 字符
        out, truncated = _truncate(text, 200)
        self.assertTrue(truncated)
        self.assertIn("00000", out)      # 头
        self.assertIn("00499", out)      # 尾

    def test_failure_observation_carries_partial_output(self):
        from zagent.tools.shell import _merge_output, _truncate
        tool = make_shell_tool(timeout=2)
        cmd = (f'"{PY}" -c "import sys,time; print(\'PARTIAL\'); '
               f'sys.stdout.flush(); time.sleep(30)"')
        r = tool.fn(command=cmd)
        self.assertFalse(r.ok)
        self.assertEqual(r.status, "timeout")
        self.assertIn("PARTIAL", r.to_observation(),
                      "失败观测里必须带上已产出的部分")


class CommandRisk(unittest.TestCase):
    def test_classification(self):
        self.assertEqual(classify_command_risk("ls -la"), Risk.EXEC)
        self.assertEqual(classify_command_risk("git status"), Risk.EXEC)
        self.assertEqual(classify_command_risk("rm -rf build"),
                         Risk.EXEC_DESTRUCTIVE)
        self.assertEqual(classify_command_risk("git reset --hard"),
                         Risk.EXEC_DESTRUCTIVE)
        self.assertEqual(classify_command_risk("git push --force origin main"),
                         Risk.IRREVERSIBLE)
        self.assertEqual(classify_command_risk("curl http://x"), Risk.NETWORK)


class SplitCommand(unittest.TestCase):
    def test_quotes_are_stripped(self):
        self.assertEqual(_split_command('python -c "print(1)"'),
                         ["python", "-c", "print(1)"])

    def test_backslashes_preserved(self):
        """Windows 路径不能被当成转义吃掉。"""
        self.assertEqual(_split_command(r"cl.exe C:\temp\x.cpp"),
                         [r"cl.exe", r"C:\temp\x.cpp"])

    def test_quoted_path_with_space(self):
        self.assertEqual(_split_command(r'"C:\Program Files\cl.exe" /nologo'),
                         [r"C:\Program Files\cl.exe", "/nologo"])


class RegistrySafety(unittest.TestCase):
    def test_unknown_tool_does_not_raise(self):
        reg = ToolRegistry()
        r = reg.call("nope", {})
        self.assertEqual(r.status, "unknown_tool")
        self.assertFalse(r.ok)

    def test_bad_args_becomes_observation(self):
        reg = ToolRegistry()
        reg.register(make_shell_tool())
        r = reg.call("shell", {"unexpected": 1})
        self.assertEqual(r.status, STATUS_BAD_ARGS)

    def test_registry_exports_schema(self):
        reg = ToolRegistry()
        reg.register(make_shell_tool())
        s = reg.schema()
        self.assertEqual(s[0]["name"], "shell")
        self.assertIn("parameters", s[0])


class PolicyTest(unittest.TestCase):
    def test_plan_mode_blocks_exec(self):
        p = Policy(mode=Mode.PLAN)
        self.assertTrue(p.decide(Risk.EXEC).need_ask)
        self.assertTrue(p.decide(Risk.READ).auto)

    def test_normal_mode_allows_local_actions(self):
        p = Policy(mode=Mode.NORMAL)
        self.assertTrue(p.decide(Risk.READ).auto)
        self.assertTrue(p.decide(Risk.EXEC).auto)
        self.assertTrue(p.decide(Risk.EXEC_DESTRUCTIVE).need_ask)

    def test_irreversible_never_auto(self):
        for mode in Mode:
            self.assertTrue(Policy(mode=mode).decide(Risk.IRREVERSIBLE).need_ask,
                            f"{mode} 档位不应自动放行不可逆操作")

    def test_low_confidence_triggers_ask(self):
        """不确定性必须真的拦住动作，而不是只写进报告。"""
        p = Policy(mode=Mode.NORMAL)
        self.assertTrue(p.decide(Risk.EXEC).auto)
        self.assertTrue(p.decide(Risk.EXEC, confidence=0.1).need_ask)

    def test_low_confidence_does_not_block_read(self):
        """但读取不该被拦 —— 不确定时更应该去查，不是停在原地。"""
        p = Policy(mode=Mode.NORMAL)
        self.assertTrue(p.decide(Risk.READ, confidence=0.0).auto)


class LoopTest(unittest.TestCase):
    def _registry(self) -> ToolRegistry:
        reg = ToolRegistry()
        reg.register(make_shell_tool(timeout=20))
        return reg

    def test_completes(self):
        reg = self._registry()
        planner = ScriptedPlanner([
            Action(tool="shell", args={"command": f'"{PY}" -c "print(7)"'}),
            Finish(answer="done"),
        ])
        out = AgentLoop(reg, planner).run("task")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.answer, "done")
        self.assertEqual(out.steps, 2)

    def test_max_steps_guard(self):
        """
        没有这道闸，拿不到结果的决策者会无限循环烧 token。

        动作必须各不相同：重复动作会先触发停滞检测（那是更早、也更有
        信息量的判据），那样就测不到这道闸了。
        """
        reg = self._registry()
        planner = ScriptedPlanner([
            Action(tool="shell", args={"command": f'"{PY}" -c "print({i})"'})
            for i in range(50)
        ])
        out = AgentLoop(reg, planner, max_steps=5).run("task")
        self.assertEqual(out.status, STATUS_MAX_STEPS)
        self.assertEqual(out.steps, 5)

    def test_finish_is_recorded_in_trajectory(self):
        """结束决策也要进轨迹，否则复现时看不出它是何时决定收尾的。"""
        reg = self._registry()
        planner = ScriptedPlanner([Finish(answer="done")])
        out = AgentLoop(reg, planner).run("t")
        self.assertEqual(out.steps, 1)
        self.assertIsInstance(out.turns[0].step, Finish)

    def test_repeat_detection(self):
        reg = self._registry()
        same = Action(tool="shell", args={"command": f'"{PY}" -c "print(1)"'})
        planner = ScriptedPlanner([same, same, same, same])
        out = AgentLoop(reg, planner).run("task")
        self.assertEqual(out.status, STATUS_STALLED)

    def test_consecutive_failure_detection(self):
        reg = self._registry()
        bad = Action(tool="shell", args={"command": "definitely-not-a-command"})
        planner = ScriptedPlanner([bad] * 10)
        out = AgentLoop(reg, planner).run("task")
        self.assertEqual(out.status, STATUS_STALLED)

    def test_shell_risk_escalation(self):
        """
        工具表写的是 EXEC，但命令内容说了算。

        只按工具名定级，等于让 `rm -rf` 和 `ls` 以同一等级通过 ——
        权限分级形同虚设。
        """
        reg = self._registry()
        loop = AgentLoop(reg, ScriptedPlanner([]))
        self.assertEqual(loop._risk_of(Action("shell", {"command": "ls"})),
                         Risk.EXEC)
        self.assertEqual(
            loop._risk_of(Action("shell", {"command": "rm -rf build"})),
            Risk.EXEC_DESTRUCTIVE)

    def test_denied_action_becomes_observation(self):
        reg = self._registry()
        planner = ScriptedPlanner([
            Action(tool="shell", args={"command": "git push --force origin main"}),
            Finish(answer="aborted"),
        ])
        out = AgentLoop(reg, planner,
                        interactor=AutoInteractor(approve_default=False)).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        denied = [t for t in out.turns if t.observation
                  and t.observation.status == STATUS_DENIED]
        self.assertTrue(denied, "未获批准必须留下可追溯的观测")

    def test_approved_action_executes(self):
        reg = self._registry()
        planner = ScriptedPlanner([
            Action(tool="shell", args={"command": "rm -rf build"}),
            Finish(answer="ok"),
        ])
        out = AgentLoop(reg, planner,
                        interactor=AutoInteractor(approve_default=True)).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertTrue(out.turns[0].observation is not None)

    def test_ask_user_is_a_step(self):
        """主动提问是决策者的一种输出，不是系统阻塞。"""
        reg = self._registry()
        planner = ScriptedPlanner([
            AskUser(question="用哪个分支?", options=["main", "dev"]),
            Finish(answer="ok"),
        ])
        out = AgentLoop(reg, planner,
                        interactor=AutoInteractor(answer="main")).run("t")
        self.assertEqual(out.status, STATUS_DONE)
        self.assertEqual(out.turns[0].answer, "main")

    def test_steering_delivered(self):
        """可中断：消息排进队列，下一轮决策前交到决策者手上。"""
        reg = self._registry()
        planner = ScriptedPlanner([
            Action(tool="shell", args={"command": f'"{PY}" -c "print(1)"'}),
            Finish(answer="ok"),
        ])
        loop = AgentLoop(reg, planner)
        loop.push("别用 rm，改用 git clean")
        loop.run("t")
        self.assertIn(["别用 rm，改用 git clean"], planner.seen_pending)


class CodeToolsTest(unittest.TestCase):
    """代码工具只做转发，行为必须与 CodeIndex 一致。"""

    class StubCode:
        def __init__(self, text: str, rev: str = "r1"):
            self.lines = text.splitlines()
            self.rev = rev

        def read_lines(self, path):
            return list(self.lines)

        def read_context(self, loc, radius=40):
            s = max(0, loc.line - 1 - radius)
            e = min(len(self.lines), loc.line + radius)
            return "\n".join(f"{i+1}: {self.lines[i]}" for i in range(s, e))

        def grep(self, pattern):
            import re
            rx = re.compile(pattern)
            return [(Path("f.cpp"), i + 1, t)
                    for i, t in enumerate(self.lines) if rx.search(t)]

        def current_revision(self):
            from zagent.types import Revision
            return Revision(id=self.rev)

    def setUp(self):
        src = "\n".join(f"line {i}" for i in range(1, 601))
        self.reg = ToolRegistry()
        for t in make_code_tools(self.StubCode(src), max_lines=50):
            self.reg.register(t)

    def test_read_lines_reports_hidden(self):
        r = self.reg.call("read_lines", {"path": "f.cpp", "count": 50})
        self.assertTrue(r.ok)
        self.assertEqual(r.data["hidden"], 550)
        self.assertIn("还有 550 行未显示", r.output)

    def test_grep_found(self):
        r = self.reg.call("grep", {"pattern": r"line 42$"})
        self.assertTrue(r.ok)
        self.assertEqual(r.data["count"], 1)
        self.assertIn(":42:", r.output)

    def test_grep_not_found_is_ok(self):
        """没匹配到不是错误 —— 那是一条有效观测。"""
        r = self.reg.call("grep", {"pattern": "zzzz"})
        self.assertTrue(r.ok)
        self.assertEqual(r.data["count"], 0)

    def test_bad_line_rejected(self):
        r = self.reg.call("read_context", {"path": "f.cpp", "line": 0})
        self.assertEqual(r.status, STATUS_BAD_ARGS)


if __name__ == "__main__":
    unittest.main()
