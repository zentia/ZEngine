"""
故障模式库的验收测试。

这些测试跑真实的编译与崩溃，不 mock —— 故障模式库的价值全在于
「这些代码真的会按预期崩」，mock 掉的测试等于没有测试。

无编译器时整组跳过（skip 而非 fail）：CI 上没装 VS 是正常的，
不应该因为环境不含编译器就让构建变红。
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zfault import process as zproc
from zfault.patterns import PATTERNS, all_patterns
from zfault.toolchain import detect

_TOOLCHAIN = detect()
# 同 test_lab.py：这里跑的是 Windows 崩溃捕获（f.exe / WER 抑制 / 退出码
# 0xFFFFFFFF），不是"能编就行"。用 supported 会在 Linux 上硬跑然后失败。
_NEEDS_CXX = pytest.mark.skipif(
    not _TOOLCHAIN.can_run_lab,
    reason=(f"需要 MSVC + Windows（崩溃捕获依赖 SEH）: {_TOOLCHAIN}"
            if _TOOLCHAIN.supported else
            f"无可用 C++ 编译器: {'; '.join(_TOOLCHAIN.notes)}"),
)


def _compile(tc, source: str, workdir: Path) -> tuple[bool, str, str]:
    src = workdir / "f.cpp"
    src.write_text(source, encoding="utf-8")
    exe = workdir / "f.exe"
    obj = workdir / "f.obj"

    cmd = [tc.cxx, "/nologo", "/std:c++17", "/EHsc", "/Zi", "/Od",
           f"/Fe:{exe}", f"/Fo:{obj}", str(src)]
    r = zproc.run(cmd, timeout=180, env=tc.build_env)
    return r.returncode == 0, r.stdout, r.stderr


# ------------------------------------------------------- 纯静态检查（不需编译器）

def test_all_patterns_have_root_cause_marker():
    """每条模式都必须能定位到根因行，否则 ground truth 无效。"""
    for p in all_patterns():
        line = p.root_cause_line()
        assert line > 0, f"{p.id} 找不到根因标记 {p.root_cause_line_marker!r}"
        # 标记必须真的在该行的注释里
        src_line = p.source.splitlines()[line - 1]
        assert p.root_cause_line_marker in src_line


def test_pattern_ids_unique():
    ids = [p.id for p in all_patterns()]
    assert len(ids) == len(set(ids))


def test_every_pattern_has_detect_hint():
    """没有 detect_hint 的模式无法用于 eval 判定。"""
    for p in all_patterns():
        assert p.detect_hint.strip(), f"{p.id} 缺少 detect_hint"


def test_negative_samples_exist():
    """样本集必须含不崩溃的负样本，否则 agent 学的只是「找崩溃」。"""
    non_crashing = [p for p in all_patterns() if p.category in ("leak", "race")]
    assert non_crashing, "缺少非崩溃类负样本"


# ------------------------------------------------------- 需要编译器的检查

@_NEEDS_CXX
@pytest.mark.parametrize("pid", sorted(PATTERNS.keys()))
def test_pattern_compiles(pid):
    """每条模式都必须能编译 —— 编译不过的模式毫无价值。"""
    p = PATTERNS[pid]
    with tempfile.TemporaryDirectory(prefix="zfl_t_") as d:
        ok, out, err = _compile(_TOOLCHAIN, p.source, Path(d))
        assert ok, f"{pid} 编译失败:\n{out}\n{err}"


@_NEEDS_CXX
@pytest.mark.parametrize("pid", sorted(PATTERNS.keys()))
def test_pattern_behaves_as_expected(pid):
    """
    验证运行时行为与声明一致。

    这是最关键的一组测试：它保证「deterministic=True 且分类为崩溃类」的模式
    真的会崩。如果这里挂了，说明注入的缺陷被编译器优化掉了，
    或者（更常见的）写法有逻辑错误根本没触发 —— 两者都必须修，不能放过。
    """
    p = PATTERNS[pid]
    with tempfile.TemporaryDirectory(prefix="zfl_t_") as d:
        wd = Path(d)
        ok, out, err = _compile(_TOOLCHAIN, p.source, wd)
        assert ok, f"{pid} 编译失败: {err}"

        r = zproc.run([str(wd / "f.exe")], timeout=60, env=_TOOLCHAIN.build_env)
        assert not r.timed_out, f"{pid} 超时未退出（可能没触发缺陷）"

        # **clean 是"没有缺陷"的类别，它本就不该崩。**
        # 把它排除在必崩之外，否则这段测试会逼着人给干净代码注入缺陷。
        # 它和 leak / race 一样属于"不崩类"，但语义更进一步：
        # 那两者是真的有缺陷（只是不崩），clean 则是压根没有问题。
        should_crash = p.category not in ("leak", "race", "clean")
        if should_crash and p.deterministic:
            assert r.crashed(), (
                f"{pid} 声明为必崩，实际 rc={r.returncode} ({r.signal_desc()})。"
                f"通常是缺陷被优化掉或触发条件写错，必须修模式本身。"
            )
        elif not should_crash:
            # 负样本：允许崩溃也允许不崩，但必须能退出
            assert r.returncode is not None, f"{pid} 未正常退出"


@_NEEDS_CXX
def test_no_wer_dialog_blocks():
    """
    回归测试：崩溃进程必须能自行退出，不能被 WER 对话框卡住。

    曾经因为直接 subprocess.run 崩溃程序，WER 弹框导致父进程无限等待，
    用户机器上不断弹出「f.exe - 应用程序错误」。根因是没设置 error mode。
    """
    p = PATTERNS["null_deref_01"]
    with tempfile.TemporaryDirectory(prefix="zfl_t_") as d:
        wd = Path(d)
        ok, _, _ = _compile(_TOOLCHAIN, p.source, wd)
        assert ok
        r = zproc.run([str(wd / "f.exe")], timeout=30, env=_TOOLCHAIN.build_env)
        assert not r.timed_out, "崩溃进程被阻塞（WER 弹框未抑制）"
        assert r.crashed()
        assert r.duration_s < 30
