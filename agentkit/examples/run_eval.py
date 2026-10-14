"""
eval 示例：跑一小批样本，看指标。

这一步的意义不在于数字好看，而在于**从此以后每次改动都能量化对比**。
没有基线就没法说「改好了」。

跑法：
    cd agentkit
    python -m examples.run_eval
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zagent.adapters import GitCodeIndex, LocalDumpSource, SimpleVersionResolver
from zagent.eval import Sample, SampleSet, evaluate
from zagent.pipeline import diagnose
from zagent.protocols import Context
from zagent.tracing import Tracer
from zagent.verify import HeuristicReasoner, RuleVerifier

sys.path.insert(0, str(Path(__file__).resolve().parent))
from minimal_run import SAMPLE_CPP, build_crash_sample, build_fake_repo  # noqa: E402


def build_repo(root: Path) -> str:
    """复用 minimal_run 的合成工程；返回 HEAD sha。"""
    build_fake_repo(root)
    if not shutil.which("git"):
        return "nogit"
    p = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                       capture_output=True, text=True, timeout=30)
    return (p.stdout or "").strip() or "nogit"


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="zagent_eval_"))
    repo = tmp / "repo"
    trace_dir = tmp / "traces"

    print("=" * 60)
    print("ZEngine Agent Kit — eval 示例")
    print("=" * 60)

    head = build_repo(repo)

    # ---- 构造样本集 ----
    # 正样本：ground truth 就是注入 bug 的那一行（actor.cpp:22）
    # 负样本：版本错配 / 无栈帧，正确行为是「不给确定结论」
    samples = SampleSet(name="demo-v1", samples=[
        Sample(id="p1", ref="", expected_path="src/actor.cpp", expected_line=22,
               note="注入 bug：WriteNameUnsafe 无空判断"),
        Sample(id="p2", ref="", expected_path="src/actor.cpp", expected_line=22,
               note="同上，栈帧更少"),
        Sample(id="n1", ref="", negative=True,
               note="版本错配，应被阻塞"),
        Sample(id="n2", ref="", negative=True,
               note="空栈，应声明无法定位"),
    ])

    # 为每个样本准备 crash 文件
    files: dict[str, Path] = {}
    for s in samples.samples:
        p = tmp / f"{s.id}.json"
        if s.id == "n1":
            build_crash_sample(p, product_version="9.9.9.111111", line=22, frames=4)
        elif s.id == "n2":
            data = {"platform": "Windows", "product_version": head,
                    "exception_type": "EXCEPTION_ACCESS_VIOLATION",
                    "stack": []}
            p.write_text(json.dumps(data), encoding="utf-8")
        else:
            frames = 4 if s.id == "p1" else 2
            build_crash_sample(p, product_version=head, line=22, frames=frames)
        files[s.id] = p

    # ---- 装配 ----
    code = GitCodeIndex(repo, project_globs=("src/",))
    ctx = Context(
        source=LocalDumpSource(files["p1"]),   # 占位，每个样本会替换
        version=SimpleVersionResolver(),
        code=code,
        reasoner=HeuristicReasoner(code),
        verifier=RuleVerifier(),
    )

    def run_fn(s: Sample):
        sample_file = files[s.id]
        c = Context(
            source=LocalDumpSource(sample_file),
            version=ctx.version,
            code=ctx.code,
            reasoner=ctx.reasoner,
            verifier=ctx.verifier,
        )
        return diagnose(str(sample_file), c, Tracer(f"eval-{s.id}", trace_dir))

    # ---- 跑 ----
    report = evaluate(samples, run_fn, repeat=2)
    print()
    print(report.render())

    print(f"\n轨迹: {trace_dir / 'traces.jsonl'}")
    print(f"临时目录: {tmp}")

    # 说明：这个 demo 规模极小，指标没有统计意义。
    # 真实用法是配 CI 批量注入故障，把样本量堆到几百个。
    print("\n注: 此 demo 仅 4 个样本，指标无统计意义，用于验证 eval 通路可用。")
    print("    真实用法：在 CI 里批量注入故障，把样本量堆到数百个。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
