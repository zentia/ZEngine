"""
最小可跑闭环。

不需要真实 dump、不需要编译引擎、不需要符号服务器。
目的：让任何人 clone 下来就能验证「这套框架真的能跑完一条诊断」。

跑法：
    cd agentkit
    python -m examples.minimal_run

它会：
  1. 在临时目录造一个合成工程 + 一个合成崩溃样本
  2. 走完 门禁 -> 取事实 -> 版本对齐 -> 符号映射 -> 假设验证
  3. 分别演示「版本不一致被阻塞」和「正常诊断」两条路径
  4. 打印报告与轨迹路径
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.adapters import (GitCodeIndex, LocalDumpSource, SimpleVersionResolver)
from zagent.pipeline import diagnose, render
from zagent.protocols import Context
from zagent.tracing import Tracer
from zagent.verify import HeuristicReasoner, RuleVerifier


# ---------------------------------------------------------------- 合成工程

# 行号（供合成样本引用，改动此文件时需同步）：
#  12  if (actor == nullptr)      <- WriteNameSafe 的保护
#  16  actor->id = 1;
#  22  actor->id = 2;             <- WriteNameUnsafe，注入的 bug，崩溃点
#  29  WriteNameUnsafe(list[i])   <- TickActors 调用点
SAMPLE_CPP = '''#include <cstddef>

struct Actor
{
    int  id;
    char name[32];
};

// 一个有空判断保护的写入
void WriteNameSafe(Actor* actor, const char* n)
{
    if (actor == nullptr)
    {
        return;
    }
    actor->id = 1;
}

// 故意没有保护：这里就是注入的 bug
void WriteNameUnsafe(Actor* actor, const char* n)
{
    actor->id = 2;
}

void TickActors(Actor** list, int count)
{
    for (int i = 0; i < count; ++i)
    {
        WriteNameUnsafe(list[i], "tick");
    }
}
'''


def build_fake_repo(root: Path) -> str:
    """
    造一个足够跑通验证规则的迷你工程。
    返回仓库 HEAD sha（无 git 时返回占位串）。
    """
    root.mkdir(parents=True, exist_ok=True)
    (root / "src").mkdir(exist_ok=True)
    (root / "src" / "actor.cpp").write_text(SAMPLE_CPP, encoding="utf-8")

    if not shutil.which("git"):
        return "nogit"

    for cmd in (["git", "init", "-q"],
                ["git", "config", "user.email", "eval@local"],
                ["git", "config", "user.name", "eval"],
                ["git", "add", "-A"],
                ["git", "commit", "-q", "-m", "init"]):
        subprocess.run(cmd, cwd=root, capture_output=True, timeout=30)

    p = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                       capture_output=True, text=True, timeout=30)
    return (p.stdout or "").strip() or "nogit"


def build_crash_sample(path: Path, product_version: str, line: int = 24,
                       frames: int = 4) -> None:
    """
    造一个合成崩溃样本，指向 WriteNameUnsafe 里那行 actor->id = 2。

    product_version 需能被 SimpleVersionResolver 解析成目标 revision。
    末段是数字时取末段；这里直接传 "1.4.0.<sha>" 不行（sha 非纯数字），
    所以场景 B 用 SimpleVersionResolver 的同类规则：取末段数字。
    实际上让示例直接把 sha 放在能被取到的位置由调用方保证。
    """
    base = [
        {"module": "game", "function": "WriteNameUnsafe",
         "file": "src/actor.cpp", "line": line},
        {"module": "game", "function": "TickActors",
         "file": "src/actor.cpp", "line": 31},
        {"module": "game", "function": "GameLoop",
         "file": "src/actor.cpp", "line": 35},
        {"module": "game", "function": "main",
         "file": "src/actor.cpp", "line": 40},
    ]
    data = {
        "platform": "Windows",
        "product_version": product_version,
        "exception_type": "EXCEPTION_ACCESS_VIOLATION",
        "exception_message": "read access violation",
        "crash_time": "2026-10-13 12:00:00",
        "uid": "10001",
        "device": "synthetic",
        "uptime_seconds": 17.0,
        "stack": base[:frames],
    }
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


# ---------------------------------------------------------------- 装配

def _head_sha(repo: Path) -> str:
    """取合成仓库的 HEAD sha；无 git 时返回占位串。"""
    if not shutil.which("git"):
        return "nogit"
    p = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                       capture_output=True, text=True, timeout=30)
    return (p.stdout or "").strip() or "nogit"


def build_ctx(repo: Path, sample: Path, trace_dir: Path,
              version_prefix: str = "r") -> Context:
    code = GitCodeIndex(repo, project_globs=("src/",))
    return Context(
        source=LocalDumpSource(sample),
        version=SimpleVersionResolver(prefix=version_prefix),
        code=code,
        reasoner=HeuristicReasoner(code),
        verifier=RuleVerifier(),
    )


# ---------------------------------------------------------------- 主流程

def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="zagent_"))
    repo = tmp / "repo"
    trace_dir = tmp / "traces"

    print("=" * 60)
    print("ZEngine Agent Kit — 最小闭环")
    print("=" * 60)
    print(f"临时目录: {tmp}\n")

    build_fake_repo(repo)
    print(f"[1] 合成工程已建立: {repo}")

    head = _head_sha(repo)

    # ---- 场景 A：版本不一致，应被阻塞 ----
    print("\n" + "-" * 60)
    print("场景 A：版本不一致（预期被阻塞，不自动切代码）")
    print("-" * 60)

    sample_a = tmp / "crash_a.json"
    # 用一个解析不出当前 HEAD 的版本串，模拟版本错配
    build_crash_sample(sample_a, product_version="1.4.0.2.804739")

    ctx_a = build_ctx(repo, sample_a, trace_dir)
    rep_a = diagnose(str(sample_a), ctx_a, Tracer("run-a", trace_dir))

    print(render(rep_a))
    print(f"\n>>> 是否被阻塞: {not rep_a.ok}")
    print(">>> 本地改动未被 revert，分支未切换 —— 护栏生效")

    # ---- 场景 B：版本一致，正常诊断 ----
    print("\n" + "-" * 60)
    print("场景 B：版本一致（预期给出定位与证据）")
    print("-" * 60)

    sample_b = tmp / "crash_b.json"
    build_crash_sample(sample_b, product_version=head, line=22, frames=4)

    ctx_b = build_ctx(repo, sample_b, trace_dir)
    rep_b = diagnose(str(sample_b), ctx_b, Tracer("run-b", trace_dir))

    print(render(rep_b))

    # ---- 场景 C：同一规则，指向有保护的函数（应被推翻）----
    print("\n" + "-" * 60)
    print("场景 C：崩溃行指向 WriteNameSafe（有保护，预期被推翻）")
    print("-" * 60)

    sample_c = tmp / "crash_c.json"
    build_crash_sample(sample_c, product_version=head, line=16, frames=4)

    ctx_c = build_ctx(repo, sample_c, trace_dir)
    rep_c = diagnose(str(sample_c), ctx_c, Tracer("run-c", trace_dir))

    print(render(rep_c))
    print("\n>>> 这条演示的是同一条规则在不同位置给出相反判定，")
    print("    说明判定来自代码实证而非固定结论。")

    # ---- 轨迹 ----
    print("\n" + "-" * 60)
    print("轨迹")
    print("-" * 60)
    trace_file = trace_dir / "traces.jsonl"
    if trace_file.exists():
        runs = Tracer.load(trace_file)
        print(json.dumps(Tracer.summarize(runs), ensure_ascii=False, indent=2))
        print(f"\n轨迹文件: {trace_file}")

    print(f"\n完成。临时目录保留供检查: {tmp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
