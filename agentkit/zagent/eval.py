"""
样本集、指标与 runner。

这套东西存在的唯一理由：能回答「改了一版之后是不是真的变好了」。
没有 eval 的 agent 项目，只能证明「做过」，不能证明「有效」。

指标里 honesty 最关键：负样本上主动声明 unknown 的比例。
一个在信息不足时说「我不知道」的系统，工程价值远高于一个总能给出错误答案的系统。
"""

from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .types import Hypothesis, Report, Verdict


# ============================================================ 样本

@dataclass
class Sample:
    """
    一个 eval 样本。

    expected 是 ground truth。negative=True 表示这是一个「不该给出确定结论」的样本，
    用来测 honesty —— 例如缺符号、版本不匹配的 dump。
    """

    id: str
    ref: str
    expected_path: str = ""
    expected_line: int | None = None
    negative: bool = False
    note: str = ""

    def expected_loc(self) -> str | None:
        if self.negative or not self.expected_path:
            return None
        return f"{self.expected_path}:{self.expected_line}"


@dataclass
class SampleSet:
    name: str = "default"
    samples: list[Sample] = field(default_factory=list)

    @classmethod
    def load(cls, path: Path) -> "SampleSet":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        return cls(
            name=data.get("name", "default"),
            samples=[Sample(**s) for s in data.get("samples", [])],
        )

    def save(self, path: Path) -> None:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(
            json.dumps({"name": self.name,
                        "samples": [s.__dict__ for s in self.samples]},
                       ensure_ascii=False, indent=2),
            encoding="utf-8")

    def __len__(self) -> int:
        return len(self.samples)


# ============================================================ 评分

@dataclass
class CaseResult:
    sample_id: str
    status: str = ""                  # ok / blocked / failed
    top1_hit: bool = False
    top3_hit: bool = False
    honest: bool = False              # 负样本上是否克制
    evidence_completeness: float = 0.0
    confidence: float = 0.0
    failure_type: str = ""


def _normalize(p: str) -> str:
    """
    归一化路径，只保留用于比对的最小充分形式。

    这里曾经踩过坑：ground truth 写的是相对路径 "src/actor.cpp"，
    而报告里是绝对路径，字符串直接比对必然判为失败——
    看起来是「定位错了」，实际只是格式不同。

    eval 里的假失败比真失败更危险：它会让你去优化一个根本没坏的东西。
    所以比对前统一取 (最后两级目录 + 文件名, 行号)。
    """
    s = str(p).replace("\\", "/").strip()

    line = ""
    if ":" in s and s.rsplit(":", 1)[-1].lstrip("-").isdigit():
        s, _, line = s.rpartition(":")

    parts = [x for x in s.split("/") if x]
    tail = "/".join(parts[-2:]) if len(parts) >= 2 else (parts[0] if parts else "")

    return f"{tail}:{line}" if line else tail


def _loc_key(loc) -> str:
    """从 SourceLocation 构造比对键（含行号）。"""
    return _normalize(f"{loc.path}:{loc.line}")


def score(sample: Sample, report: Report, topk: int = 3) -> CaseResult:
    """对单个样本评分。"""
    r = CaseResult(sample_id=sample.id, status=report.status,
                   confidence=report.confidence)

    # --- 负样本：测的是克制，不是能力 ---
    if sample.negative:
        # 诚实 = 没给出高置信度结论（被阻塞、或置信度低、或主动声明 unknown）
        r.honest = (
            report.status == "blocked"
            or report.confidence <= 0.35
            or not report.conclusions
            or bool(report.unknowns)
        )
        if not r.honest:
            r.failure_type = "负样本仍给出高置信结论"
        return r

    # --- 正样本：测的是定位能力 ---
    expected = sample.expected_loc()
    if expected is None:
        r.failure_type = "样本缺少 ground truth"
        return r

    cands = [_normalize(str(x)) for x in report.top(topk)]
    exp = _normalize(expected)

    r.top1_hit = bool(cands) and cands[0] == exp
    r.top3_hit = exp in cands

    # 证据完备率
    evs = [e for h in report.conclusions for e in h.evidence]
    r.evidence_completeness = (
        sum(1 for e in evs if e.is_traceable) / len(evs) if evs else 0.0
    )

    # 失败分类
    if report.status == "blocked":
        r.failure_type = "被阻塞: " + report.block_reason[:60]
    elif not report.conclusions:
        r.failure_type = "无结论"
    elif not r.top3_hit:
        r.failure_type = "定位错误"
    elif r.evidence_completeness < 0.5:
        r.failure_type = "证据不可回溯"

    return r


@dataclass
class EvalReport:
    name: str = ""
    total: int = 0
    top1: float = 0.0
    top3: float = 0.0
    honesty: float = 0.0
    evidence_completeness: float = 0.0
    blocked: int = 0
    stability: float = 0.0
    stability_n: int = 0            # 参与稳定性统计的样本数；0 = 未测量
    failures: list[CaseResult] = field(default_factory=list)

    def render(self) -> str:
        lines = [
            f"eval: {self.name}   samples: {self.total}",
            "",
            f"  top1            {self.top1:.2%}",
            f"  top3            {self.top3:.2%}",
            f"  honesty         {self.honesty:.2%}   <- 负样本上的克制率，最关键",
            f"  evidence        {self.evidence_completeness:.2%}",
            f"  blocked         {self.blocked}",
            f"  stability       "
            + (f"{self.stability:.2%}" if self.stability_n
               else "n/a (repeat<=1, 未测量)"),
        ]
        if self.failures:
            lines += ["", "失败明细:"]
            for f in self.failures[:20]:
                lines.append(f"  [{f.sample_id}] {f.failure_type}")
        return "\n".join(lines)


def evaluate(samples: SampleSet, run_fn, repeat: int = 1) -> EvalReport:
    """
    跑一整轮 eval。

    run_fn(sample) -> Report，由调用方注入（这样 eval 与具体装配解耦）。
    repeat > 1 时额外计算稳定性：同一样本多次跑，结论是否一致。
    """
    positives = [s for s in samples.samples if not s.negative]
    negatives = [s for s in samples.samples if s.negative]

    hits1 = hits3 = 0
    honest = 0
    ev_sum = 0.0
    ev_n = 0
    blocked = 0
    failures: list[CaseResult] = []

    stability_hits = 0
    stability_n = 0

    for s in samples.samples:
        report = run_fn(s)
        r = score(s, report)

        if report.status == "blocked":
            blocked += 1

        if s.negative:
            honest += int(r.honest)
            if not r.honest:
                failures.append(r)
        else:
            hits1 += int(r.top1_hit)
            hits3 += int(r.top3_hit)
            ev_sum += r.evidence_completeness
            ev_n += 1
            if not r.top3_hit or r.evidence_completeness < 0.5:
                failures.append(r)

        # 稳定性：重跑看结论位置是否一致
        if repeat > 1:
            locs = []
            for _ in range(repeat):
                rep2 = run_fn(s)
                locs.append(str(rep2.location) if rep2.location else "")
            if len(set(locs)) == 1:
                stability_hits += 1
            stability_n += 1

    out = EvalReport(
        name=samples.name,
        total=len(samples.samples),
        top1=hits1 / len(positives) if positives else 0.0,
        top3=hits3 / len(positives) if positives else 0.0,
        honesty=honest / len(negatives) if negatives else 0.0,
        evidence_completeness=ev_sum / ev_n if ev_n else 0.0,
        blocked=blocked,
        stability=stability_hits / stability_n if stability_n else 0.0,
        stability_n=stability_n,
        failures=failures,
    )
    return out
