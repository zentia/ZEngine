"""
结构化轨迹。

没有这个，就只能说「我跑过了」；有了它才能回答「这次为什么错」以及
「改了一版之后是不是真的变好了」。

落盘格式 JSONL：一行一个 run，便于 jq / pandas 直接分析。
"""

from __future__ import annotations

import json
import threading
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from .types import Hypothesis, Verdict, now_ms


@dataclass
class Stage:
    name: str
    status: str = "ok"           # ok / degraded / blocked / skipped
    t_ms: int = 0
    data: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {"name": self.name, "status": self.status,
                "t_ms": self.t_ms, **self.data}


class Tracer:
    """单次执行的轨迹收集器。线程安全，便于并发跑 eval。"""

    def __init__(self, run_id: str, out_dir: Path | None = None) -> None:
        self.run_id = run_id
        self.out_dir = out_dir
        self._stages: list[Stage] = []
        self._notes: list[str] = []
        self._lock = threading.Lock()
        self._marks: dict[str, int] = {}

    # ---- 阶段计时 ----

    def begin(self, name: str) -> None:
        with self._lock:
            self._marks[name] = now_ms()

    def end(self, name: str, status: str = "ok", **data: Any) -> None:
        with self._lock:
            t0 = self._marks.pop(name, now_ms())
            self._stages.append(Stage(name, status, now_ms() - t0, data))

    def stage(self, name: str, status: str = "ok", **data: Any) -> None:
        """不计时的瞬时阶段。"""
        with self._lock:
            self._stages.append(Stage(name, status, 0, data))

    def note(self, text: str) -> None:
        with self._lock:
            self._notes.append(text)

    # ---- 输出 ----

    def to_dict(self, outcome: dict[str, Any] | None = None,
                ledger: list[Hypothesis] | None = None) -> dict[str, Any]:
        ledger_out = []
        for h in ledger or []:
            ledger_out.append({
                "id": h.id,
                "kind": h.kind,
                "claim": h.claim,
                "verdict": h.verdict.value,
                "evidence": [
                    {"method": e.method, "locator": e.locator, "detail": e.detail}
                    for e in h.evidence
                ],
            })

        return {
            "run_id": self.run_id,
            "ts": datetime.now().isoformat(timespec="seconds"),
            "stages": [s.to_dict() for s in self._stages],
            "notes": list(self._notes),
            "hypothesis_ledger": ledger_out,
            "outcome": outcome or {},
        }

    def flush(self, outcome: dict[str, Any] | None = None,
              ledger: list[Hypothesis] | None = None) -> Path | None:
        if self.out_dir is None:
            return None
        self.out_dir.mkdir(parents=True, exist_ok=True)
        path = self.out_dir / "traces.jsonl"
        with path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(self.to_dict(outcome, ledger), ensure_ascii=False) + "\n")
        return path

    # ---- 汇总（供 eval 用）----

    @staticmethod
    def load(path: Path) -> list[dict[str, Any]]:
        if not path.exists():
            return []
        out = []
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line:
                out.append(json.loads(line))
        return out

    @staticmethod
    def summarize(runs: list[dict[str, Any]]) -> dict[str, Any]:
        """对一批 run 做粗统计。"""
        if not runs:
            return {"runs": 0}

        verdict_count = {v.value: 0 for v in Verdict}
        blocked = 0
        for r in runs:
            if r.get("outcome", {}).get("status") == "blocked":
                blocked += 1
            for h in r.get("hypothesis_ledger", []):
                v = h.get("verdict")
                if v in verdict_count:
                    verdict_count[v] += 1

        return {
            "runs": len(runs),
            "blocked": blocked,
            "verdicts": verdict_count,
            "avg_confidence": round(
                sum(r.get("outcome", {}).get("confidence", 0.0) for r in runs) / len(runs), 3
            ),
        }
