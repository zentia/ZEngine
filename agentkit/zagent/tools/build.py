"""
把「构建 / 测试」暴露成**会发裁定**的工具。这是编辑器 AI 的验证落点。

为什么不让 agent 直接调 shell 跑 zbuild.py
------------------------------------------
shell 也会返回结果，但**结果里没有裁定**。agent 读到退出码 0 之后，
"构建通过"这句话仍然是它自己说的 —— 那正是 `verify_tool.py` 里写过的
那句：玩具 agent 的"验证"是模型自己再说一遍"我确认了"，等于没验证。

差别只在一点：裁定由**工具**做出（退出码 + 输出证据），写进
`data.verdict`，交给 `ConclusionGate` 在 finish 处核对。agent 无法给
自己发 CONFIRMED。

而且这样做门禁**一行都不用改**：`collect_claims` 扫描任意工具 `data` 里的
`verdict` / `conclusions`，它根本不认识 `build_project`。当初"不认识
具体工具"这个抽象，就是为了今天能直接复用。

退出码 0 不等于验证过
--------------------
这是本工具最要紧、也最容易做错的一条。三种"看起来成功、其实没验证"：

1. 构建无事可做（`up-to-date` / `Nothing to be done`）→ 退出 0
2. 测试收集到 0 个用例 → 退出 0
3. 目标拼错、构建系统没匹配到任何东西 → 有些仍退出 0

所以把"退出 0"拆成两档：
- 退出 0 **且**输出里有正面证据 → `confirmed`
- 退出 0 但读不出证据 → `unproven`，并**明确说出为什么**

宁可让 agent 多跑一次，也不能让它拿一个空结果当证明。`unproven` 进不了
结论（`Claim.allowed` 要求 CONFIRMED），于是门禁会拦住它。

为什么 claim 要短
-----------------
`gate.matches` 拿模型声明的结论与这里的 claim 做匹配，短声明走的是
覆盖率判据，且要求 `min(len) >= 6`。所以 claim 必须是**短而规范**的短语
（"构建通过"），细节放 `locators`。写成
"构建成功（zbuild.py build，退出码 0）"这种长句，模型一句"构建通过"
反而匹配不上 —— 那会造成"明明跑了构建却被驳回"的假阳性。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from .base import (STATUS_BAD_ARGS, STATUS_ERROR, STATUS_OK, Risk, Tool,
                   ToolResult)

CONFIRMED = "confirmed"
REFUTED = "refuted"
UNPROVEN = "unproven"


@dataclass
class BuildRun:
    """一次构建/测试的执行明细。与具体执行方式解耦，便于注入假实现。"""

    returncode: int | None = None
    output: str = ""
    status: str = STATUS_OK
    elapsed_s: float = 0.0
    produced_output: bool = False


# ---------------------------------------------------------------- 证据识别

# 构建"真的做了事"的反面信号。命中即视为"没验证到东西"。
_NO_WORK_RE = re.compile(
    r"up[- ]?to[- ]?date|Nothing to be done|No work to do|"
    r"无需重新构建|没有可做的|nothing to build|no targets",
    re.IGNORECASE,
)

# 测试"实际跑了多少用例"。读不出来 = 没有证据。
_TEST_COUNT_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"(\d+)\s+passed", re.IGNORECASE),                  # pytest
    re.compile(r"\[\s*PASSED\s*\]\s*(\d+)\s+test", re.IGNORECASE),  # gtest
    re.compile(r"(\d+)\s+tests?\s+passed", re.IGNORECASE),
    re.compile(r"Passed:\s*(\d+)", re.IGNORECASE),                  # dotnet
    re.compile(r"通过[^\d]{0,12}?(\d+)\s*(?:个)?(?:用例|测试)"),
    re.compile(r"(\d+)\s*(?:个)?(?:用例|测试)[^\d]{0,12}?通过"),
)


def test_evidence(output: str) -> str | None:
    """
    从测试输出里读"实际跑了多少用例"。

    **读到 0 个返回 None**：0 个用例通过是最危险的假成功 —— 退出码是 0，
    什么都没测，而 agent 会当成"全绿"。这里必须把它判成没有证据。
    """
    for pat in _TEST_COUNT_RES:
        m = pat.search(output or "")
        if m:
            try:
                n = int(m.group(1))
            except (TypeError, ValueError):
                continue
            if n > 0:
                return f"实际跑过 {n} 个用例"
    return None


def build_evidence(output: str) -> str | None:
    """
    构建"真的编译了东西"的证据。

    构建系统千差万别，这里不敢假装能识别所有成功信号。只做两件稳的事：
    - 命中"无事可做"-> 明确没有证据
    - 有实质输出 -> 视为有证据（细节交给 locators 里的退出码与耗时）

    弱是弱的，但**诚实**：宁可标 unproven 让 agent 再跑一次，也不能
    把一次 no-op 当成构建通过。
    """
    text = output or ""
    if not text.strip():
        return None
    if _NO_WORK_RE.search(text):
        return None
    return "构建产出了输出且退出码为 0"


def _truncate(text: str, limit: int) -> tuple[str, bool]:
    # 与 shell._truncate 同策略：**保留头尾**。编译器把真正的错误写在
    # 最后，测试框架把失败汇总写在最后 —— 只留开头等于丢掉结论。
    if not limit or len(text) <= limit:
        return text, False
    keep = max(1, limit // 2 - 40)
    dropped = len(text) - 2 * keep
    return (text[:keep] + f"\n...[截断 {dropped} 字符]...\n" + text[-keep:],
            True)


def _subprocess_runner(command: str, cwd: str, timeout: float) -> BuildRun:
    """默认执行器：复用 shell.execute，那里已经解决了挂死的五类成因。"""
    from .shell import execute

    r = execute(command=command, cwd=cwd, timeout=timeout)
    merged = r.stdout
    if r.stderr:
        merged += (("\n" if merged and not merged.endswith("\n") else "")
                   + r.stderr)
    return BuildRun(returncode=r.returncode, output=merged, status=r.status,
                    elapsed_s=r.elapsed_s, produced_output=r.produced_output)


# ---------------------------------------------------------------- 工具

def make_build_tools(root: str | Path,
                     build_cmd: str = "python zbuild.py build",
                     test_cmd: str = "python zbuild.py test",
                     runner: Callable[[str, str, float], BuildRun] | None = None,
                     timeout: float = 1800.0,
                     max_output_chars: int = 8000) -> list[Tool]:
    """
    造构建/测试工具。

    runner 可注入
        默认是真跑子进程。测试与 eval 注入假实现 —— 构建真的一次要
        几十分钟，而验证"agent 有没有跑构建"这件事根本不需要真构建。
        把执行器做成参数，这个工具才能被测试。

    timeout 默认 30 分钟
        构建慢是常态，而"超时被杀"会让 agent 换一个更蠢的实现绕过去。
        宁可等。
    """
    root_path = Path(root)
    run = runner or _subprocess_runner
    ledger: list[dict[str, Any]] = []

    def _verify(kind: str, command: str, claim: str,
                evidence_fn: Callable[[str], str | None]) -> ToolResult:
        if not root_path.exists():
            return ToolResult(
                ok=False, status=STATUS_ERROR,
                error=f"工程根目录不存在: {root_path}")

        r = run(command, str(root_path), timeout)
        out, truncated = _truncate(r.output, max_output_chars)

        # ---- 裁定。这一块决定一切，必须由工具做出，不能由模型做出。
        if r.status != STATUS_OK or r.returncode != 0:
            verdict = REFUTED
            why = (f"命令已执行完毕，但退出码为 {r.returncode}"
                   if r.returncode is not None
                   else f"未能正常完成（{r.status}）")
        else:
            evidence = evidence_fn(r.output)
            if evidence:
                verdict = CONFIRMED
                why = evidence
            else:
                verdict = UNPROVEN
                why = ("退出码为 0，但输出里读不出'真的验证过'的证据"
                       "（可能是无事可做，或收集到 0 个用例）。"
                       "这不能当作验证通过。")

        locator = f"{command} (exit={r.returncode}, {r.elapsed_s:.1f}s)"
        entry = {"kind": kind, "claim": claim, "verdict": verdict,
                 "why": why, "locator": locator}
        ledger.append(entry)

        lines = [f"裁定: {verdict}", f"{kind}: {claim}",
                 f"依据: {why}", f"来源: {locator}"]
        if verdict != CONFIRMED and out:
            lines.append("")
            lines.append("输出（供你纠偏）:")
            lines.append(out)

        return ToolResult(
            # 三种裁定都是"验证成功完成"。REFUTED 不是工具故障 ——
            # 把它回成 ok=False，模型会当成出错去重试，恰恰是把正确
            # 结论当成失败（verify_tool.py 里踩过同一条）。
            ok=True,
            output="\n".join(lines),
            status=verdict,
            elapsed_s=r.elapsed_s,
            truncated=truncated,
            data={
                "verdict": verdict,
                # 多条式：claim 短而规范，细节进 locators。
                # 长 claim 会让"构建通过"这种短声明匹配不上。
                "conclusions": [{"claim": claim, "verdict": verdict,
                                 "locators": [locator]}],
                "kind": kind,
                "command": command,
                "returncode": r.returncode,
                "elapsed_s": r.elapsed_s,
                "why": why,
            },
        )

    def _build() -> ToolResult:
        return _verify("构建", build_cmd, "构建通过", build_evidence)

    def _test() -> ToolResult:
        return _verify("测试", test_cmd, "测试通过", test_evidence)

    def _ledger() -> ToolResult:
        if not ledger:
            return ToolResult(
                ok=True,
                output="还没有跑过任何构建或测试 —— 现在没有任何可进结论的验证。",
                data={"verified": [], "allowed": []})
        allowed = [e["claim"] for e in ledger if e["verdict"] == CONFIRMED]
        lines = [f"已跑 {len(ledger)} 次验证。可进结论（CONFIRMED）: "
                 f"{', '.join(allowed) if allowed else '（无）'}", ""]
        for e in ledger:
            lines.append(f"  [{e['verdict']}] {e['claim']}  <- {e['locator']}"
                         f"\n      {e['why']}")
        return ToolResult(ok=True, output="\n".join(lines),
                          data={"verified": list(ledger), "allowed": allowed})

    return [
        Tool(
            name="build_project",
            description=(
                "构建整个工程，返回 CONFIRMED / REFUTED / UNPROVEN。"
                "**裁定由工具做出，不由你做出**：退出码非 0 是 REFUTED；"
                "退出码 0 但读不出'真的编译了东西'的证据是 UNPROVEN；"
                "只有退出码 0 且有证据才是 CONFIRMED。"
                "只有 CONFIRMED 能写进结论。REFUTED 是有价值的结果，"
                "不要当成工具出错去重试。"
            ),
            risk=Risk.EXEC,
            params={"type": "object", "properties": {}},
            fn=_build,
        ),
        Tool(
            name="run_tests",
            description=(
                "跑工程测试，返回 CONFIRMED / REFUTED / UNPROVEN。"
                "**收集到 0 个用例判为 UNPROVEN**：退出码是 0 但什么都没测，"
                "这是最危险的假成功。想让结论里有'测试通过'，就必须真的"
                "跑过用例。"
            ),
            risk=Risk.EXEC,
            params={"type": "object", "properties": {}},
            fn=_test,
        ),
        Tool(
            name="verification_ledger",
            description=(
                "查看已跑过的构建/测试台账：哪些可进结论、哪些被判 UNPROVEN"
                "及原因。收尾前调一次，确认结论里每一条都有 CONFIRMED 支撑。"
            ),
            risk=Risk.READ,
            params={"type": "object", "properties": {}},
            fn=_ledger,
        ),
    ]
